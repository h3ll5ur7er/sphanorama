// V7 — how frames are aligned. This file covers feature extraction only; matching and the global
// refinement arrive in their own increments.
//
// Feature counts are not knowable in advance and would be meaningless if they were: how many
// corners ORB finds on a checkerboard depends on its threshold, the pyramid, and the content's
// contrast. So these assert invariants — a floor, a determinism, a refusal, an ordering between
// textured and blank — rather than numbers, which is the shape the engineering skill asks for and
// the shape that survives a detector being retuned.
//
// The whole file is conditional on OpenCV (ADR 0052). A WASM-only checkout has no registration and
// therefore no registration tests, which the CMake comment says where a reader will look for them.
#include <gtest/gtest.h>

#include <opencv2/core.hpp>
#include <opencv2/features2d.hpp>

#include <algorithm>
#include <cstdint>
#include <vector>

#include "engines/registration_engine/feature_registration_engine.h"
#include "engines/registration_engine/null_registration_engine.h"
#include "resource_access/frame_store_access/memory_frame_store_access.h"

namespace sphanorama {
namespace {

constexpr int32_t kWidth = 128;
constexpr int32_t kHeight = 128;
constexpr int32_t kTile = 16;

/**
 * A checkerboard with a quadratic ramp on top: corners for a corner detector, gradients for a blob
 * detector, and no two tiles alike, so a matcher cannot be fooled by repetition.
 *
 * The ramp is quadratic rather than the obvious `(x + y) / 4` because a ramp in `x + y` is constant
 * along every anti-diagonal, which makes the two tiles facing each other across one byte-for-byte
 * equal. 49 of these 64 tiles repeated an earlier tile under that ramp, while the comment beside it
 * claimed none did — so the claim is a test below rather than a sentence here.
 */
uint8_t TexturedLuma(int32_t x, int32_t y) {
  const int32_t tile = ((x / kTile) + (y / kTile)) % 2 == 0 ? 40 : 210;
  return static_cast<uint8_t>((tile + (x * x + y * y * 3) / 16) % 256);
}

class Extraction : public ::testing::TestWithParam<FeatureDetector> {
 protected:
  MemoryFrameStoreAccess store{1 << 24};

  FeatureRegistrationEngine Engine() { return FeatureRegistrationEngine{store, GetParam()}; }

  /**
   * A frame of `edge` square whose RGBA8 luma at (x, y) is whatever `paint` says.
   *
   * Void with an out-parameter rather than returning the frame, because the assertions have to be
   * `ASSERT_*` and `ASSERT_*` cannot appear in a function that returns a value. With `EXPECT_*` a
   * refused `Allocate` left `pinned.value` an empty span and the loop wrote 65,536 pixels through
   * its null `data()` — killing the binary with no `[  FAILED  ]` line, which is exactly the
   * crash-reads-as-clean case this file's own comments warn about. The tight-ceiling fixtures below
   * make a refused `Allocate` an ordinary event rather than a hypothetical.
   */
  template <typename Painter>
  void Paint(FrameRef* out, Painter paint, int32_t edge = kWidth) {
    ASSERT_NE(out, nullptr);
    const Result<FrameRef> allocated = store.Allocate(edge, edge, PixelFormat::RGBA8);
    ASSERT_TRUE(allocated.ok()) << allocated.status.detail;
    const FrameRef frame = allocated.value;
    const Result<std::span<uint8_t>> pinned = store.Pin(frame);
    ASSERT_TRUE(pinned.ok()) << pinned.status.detail;
    ASSERT_NE(pinned.value.data(), nullptr);
    for (int32_t y = 0; y < edge; ++y) {
      for (int32_t x = 0; x < edge; ++x) {
        const uint8_t v = paint(x, y);
        uint8_t* px = pinned.value.data() + static_cast<size_t>(y) * frame.stride + x * 4;
        px[0] = px[1] = px[2] = v;
        px[3] = 255;
      }
    }
    EXPECT_TRUE(store.Release(frame).ok());
    *out = frame;
  }

  /** Both frames a successful extraction hands over, forgotten as the contract says they must be. */
  void ForgetOutputs(const FeatureSet& features) {
    if (features.count == 0) return;
    EXPECT_TRUE(store.Forget(features.descriptors).ok());
    EXPECT_TRUE(store.Forget(features.keypoints).ok());
  }

  int64_t HeapUsed() {
    const Result<FrameStoreBudget> budget = store.Budget();
    EXPECT_TRUE(budget.ok()) << budget.status.detail;
    return budget.value.heapUsedBytes;
  }

  FrameRef Textured(int32_t edge = kWidth) {
    FrameRef frame;
    Paint(&frame, TexturedLuma, edge);
    return frame;
  }

  /** The same detector, made independently of the engine's own `Make()`. */
  static cv::Ptr<cv::Feature2D> OpenCvDetector(FeatureDetector detector) {
    switch (detector) {
      case FeatureDetector::Orb:   return cv::ORB::create();
      case FeatureDetector::Akaze: return cv::AKAZE::create();
      case FeatureDetector::Sift:  return cv::SIFT::create();
    }
    return {};
  }

  FrameRef Blank() {
    FrameRef frame;
    Paint(&frame, [](int32_t, int32_t) -> uint8_t { return 128; });
    return frame;
  }
};

TEST_P(Extraction, FindsFeaturesOnTexturedContent) {
  FeatureRegistrationEngine engine = Engine();
  const Result<FeatureSet> features = engine.ExtractFeatures(Textured());

  ASSERT_TRUE(features.ok()) << features.status.detail;
  EXPECT_GT(features.value.count, 0) << "a checkerboard has corners; a detector that finds none "
                                        "cannot register anything";
}

TEST_P(Extraction, FindsNothingOnAFlatFrame) {
  // The invariant that separates "found features" from "returned a number". A frame with no
  // gradient anywhere has nothing to find, and a detector reporting features on it is reporting
  // noise — which would then be matched against noise in the next frame.
  FeatureRegistrationEngine engine = Engine();
  const Result<FeatureSet> features = engine.ExtractFeatures(Blank());

  ASSERT_TRUE(features.ok()) << features.status.detail;
  EXPECT_EQ(features.value.count, 0);
}

TEST_P(Extraction, IsDeterministic) {
  // Two extractions of the same pixels agree bit for bit. Without this a registration run is not
  // reproducible, and neither is any accuracy number measured from one.
  FeatureRegistrationEngine engine = Engine();
  const FrameRef frame = Textured();

  const Result<FeatureSet> first = engine.ExtractFeatures(frame);
  const Result<FeatureSet> second = engine.ExtractFeatures(frame);
  ASSERT_TRUE(first.ok()) << first.status.detail;
  ASSERT_TRUE(second.ok()) << second.status.detail;
  ASSERT_EQ(first.value.count, second.value.count);

  const Result<std::span<uint8_t>> a = store.Pin(first.value.descriptors);
  const Result<std::span<uint8_t>> b = store.Pin(second.value.descriptors);
  ASSERT_TRUE(a.ok()) << a.status.detail;
  ASSERT_TRUE(b.ok()) << b.status.detail;
  ASSERT_EQ(a.value.size(), b.value.size());
  EXPECT_TRUE(std::equal(a.value.begin(), a.value.end(), b.value.begin()));
  store.Release(first.value.descriptors);
  store.Release(second.value.descriptors);
}

TEST_P(Extraction, TheDescriptorFrameHoldsExactlyTheDescriptorsItClaims) {
  // ADR 0051's shape, asserted rather than assumed: `count` rows of the detector's descriptor
  // width, tightly packed. A caller reading these bytes has only `count` and the frame to go on.
  FeatureRegistrationEngine engine = Engine();
  const Result<FeatureSet> features = engine.ExtractFeatures(Textured());
  ASSERT_TRUE(features.ok()) << features.status.detail;
  ASSERT_GT(features.value.count, 0);

  const FrameRef descriptors = features.value.descriptors;
  EXPECT_EQ(descriptors.height, features.value.count);
  EXPECT_EQ(descriptors.format, PixelFormat::Gray8);
  EXPECT_EQ(descriptors.stride, descriptors.width) << "tightly packed, per ADR 0051";

  // What the row width should be, asked of OpenCV rather than recomputed from the engine's own
  // expression. Without this the test is satisfied by any self-consistent width: an engine that
  // sized rows by `descriptors.cols` and forgot `elemSize()` would allocate 128-byte SIFT rows,
  // copy 128 bytes into each, and agree with every other assertion in this test. It also re-states
  // the detector mapping independently, so a `Make()` that handed back the wrong detector is caught.
  const cv::Ptr<cv::Feature2D> reference = OpenCvDetector(GetParam());
  ASSERT_TRUE(reference) << "no reference detector for this parameter";
  EXPECT_EQ(descriptors.width,
            static_cast<int32_t>(reference->descriptorSize()) *
                static_cast<int32_t>(CV_ELEM_SIZE(reference->descriptorType())));

  const Result<std::span<uint8_t>> pinned = store.Pin(descriptors);
  ASSERT_TRUE(pinned.ok()) << pinned.status.detail;
  EXPECT_EQ(pinned.value.size(),
            static_cast<size_t>(descriptors.width) * descriptors.height);
  store.Release(descriptors);
}

TEST_P(Extraction, RefusesAFrameTheStoreDoesNotHave) {
  // A `FrameRef` is a plain value a caller passes in, so it can name a frame that never existed.
  FeatureRegistrationEngine engine = Engine();
  FrameRef stranger;
  stranger.width = kWidth;
  stranger.height = kHeight;
  stranger.format = PixelFormat::RGBA8;

  const Result<FeatureSet> features = engine.ExtractFeatures(stranger);
  ASSERT_FALSE(features.ok());
  // The code, not just the refusal: `stranger` is a valid RGBA8 frame in every respect except that
  // the store has never heard of it, so the only thing entitled to turn it away is `Pin`. Asserting
  // only `!ok()` would pass just as well if a future format or dimension check refused it first,
  // and this test would then no longer be about an unknown frame at all.
  EXPECT_EQ(features.status.code, StatusCode::NotFound) << features.status.detail;
}

TEST_P(Extraction, RefusesAFrameClaimingMorePixelsThanTheStoreHolds) {
  // A `FrameRef` is a plain value the caller passes in, and `Pin` resolves it by `id` alone — so
  // the width, height and stride are the *caller's* account of a frame and nothing upstream makes
  // them describe the allocation. Without a check, every read below indexes the store's real span
  // with that account. `SharpnessFrameQualityEngine` has the same guard for the same reason, and a
  // reviewer reproduced this exact overflow under AddressSanitizer there before it did.
  FeatureRegistrationEngine engine = Engine();
  FrameRef inflated = Textured();
  inflated.height = kHeight + 1;

  const Result<FeatureSet> features = engine.ExtractFeatures(inflated);
  ASSERT_FALSE(features.ok());
  EXPECT_EQ(features.status.code, StatusCode::InvalidArgument) << features.status.detail;
}

TEST_P(Extraction, RefusesAStrideNarrowerThanOneRow) {
  // Not because it reads out of bounds — it does not. Rows that overlap are not a frame anybody
  // allocated. It also matters that *we* refuse it rather than OpenCV: `cv::Mat` asserts its step
  // against the row and **throws**, and the core is built `-fno-exceptions`, so an OpenCV refusal
  // here is a `terminate` where the contract promises a `Result`.
  FeatureRegistrationEngine engine = Engine();
  FrameRef sheared = Textured();
  sheared.stride = 4;

  const Result<FeatureSet> features = engine.ExtractFeatures(sheared);
  ASSERT_FALSE(features.ok());
  EXPECT_EQ(features.status.code, StatusCode::InvalidArgument) << features.status.detail;
}

TEST_P(Extraction, TheKeypointFrameHoldsACoordinatePairPerFeature) {
  // The other half of ADR 0051's shape, which nothing asserted. A reviewer zeroed every coordinate,
  // handed back `features.keypoints = FrameRef{}` and dropped `features.frame`, and the whole file
  // stayed green. The keypoint-to-descriptor row correspondence is what matching will be built on,
  // so it is worth more than the descriptors on their own.
  FeatureRegistrationEngine engine = Engine();
  const FrameRef source = Textured();
  const Result<FeatureSet> features = engine.ExtractFeatures(source);
  ASSERT_TRUE(features.ok()) << features.status.detail;
  ASSERT_GT(features.value.count, 0);
  EXPECT_EQ(features.value.frame.value, source.id.value)
      << "a FeatureSet that does not name the frame it came from cannot be matched against another";

  const FrameRef keypoints = features.value.keypoints;
  EXPECT_NE(keypoints.id.value, 0U) << "a handle nobody can resolve is not an answer";
  EXPECT_EQ(keypoints.height, features.value.count);
  EXPECT_EQ(keypoints.width, 8) << "two float32s, x then y";
  EXPECT_EQ(keypoints.stride, keypoints.width) << "tightly packed, per ADR 0051";
  EXPECT_EQ(keypoints.format, PixelFormat::Gray8);
  EXPECT_NE(keypoints.id.value, features.value.descriptors.id.value)
      << "two frames, not the same one twice";

  const Result<std::span<uint8_t>> pinned = store.Pin(keypoints);
  ASSERT_TRUE(pinned.ok()) << pinned.status.detail;
  ASSERT_EQ(pinned.value.size(), static_cast<size_t>(features.value.count) * 8);

  // Every coordinate lands inside the frame it was found in, and they are not all the same point —
  // which is what zeroing the buffer would leave behind.
  int32_t distinct = 0;
  float firstX = 0.0F;
  float firstY = 0.0F;
  for (int32_t row = 0; row < features.value.count; ++row) {
    float xy[2] = {0.0F, 0.0F};
    std::memcpy(xy, pinned.value.data() + static_cast<size_t>(row) * 8, sizeof(xy));
    EXPECT_GE(xy[0], 0.0F);
    EXPECT_GE(xy[1], 0.0F);
    EXPECT_LE(xy[0], static_cast<float>(kWidth));
    EXPECT_LE(xy[1], static_cast<float>(kHeight));
    if (row == 0) {
      firstX = xy[0];
      firstY = xy[1];
    } else if (xy[0] != firstX || xy[1] != firstY) {
      ++distinct;
    }
  }
  EXPECT_GT(distinct, 0) << "every keypoint at the same coordinate is a zeroed buffer, not features";

  EXPECT_TRUE(store.Release(keypoints).ok());
  ForgetOutputs(features.value);
}

TEST_P(Extraction, NoDetectorReturnsMoreFeaturesThanTheBudget) {
  // Two of the three are unbounded by default: `cv::SIFT::create()` takes `nfeatures = 0` meaning
  // retain everything, and `cv::AKAZE::create()` takes `max_points = -1` meaning the same. The cap
  // bounds the store, and it is also what makes "which detector wins" a measurement rather than a
  // comparison between one asked for 500 features and another asked for all of them.
  //
  // **The frame is 768 square because at 128 this test could not fail.** Uncapped on this texture
  // at 128, SIFT returns 99 and AKAZE 107 — both under the cap, so the assertion held no matter
  // what the engine did. At 768 they return 1,328 and 2,547. Counted before this test was believed.
  //
  // For ORB it is trivially true and stays here only so the suite covers all three: OpenCV's ORB
  // has always capped itself, and `nfeatures = 0` means *zero* features to it rather than
  // unlimited, so there is no uncapped ORB for this to be measured against.
  FeatureRegistrationEngine engine = Engine();
  const Result<FeatureSet> features = engine.ExtractFeatures(Textured(768));
  ASSERT_TRUE(features.ok()) << features.status.detail;
  EXPECT_LE(features.value.count, 500);
  if (GetParam() != FeatureDetector::Orb) {
    EXPECT_GT(features.value.count, 0) << "a frame this size has features; a zero here means the "
                                          "cap was applied as a floor rather than a ceiling";
  }
  ForgetOutputs(features.value);
}

TEST_P(Extraction, ReleasesTheFrameItWasGivenWhetherItSucceedsOrRefuses) {
  // "Released on every path out" is what the `Unpin` holder promises, and nothing asked. A reviewer
  // deleted the holder outright and every test in this file still passed. `Release` on an unpinned
  // frame answers `FailedPrecondition`, so an extra one is a question the store can answer: if the
  // engine left its pin behind, this succeeds instead.
  FeatureRegistrationEngine engine = Engine();

  const FrameRef good = Textured();
  const Result<FeatureSet> features = engine.ExtractFeatures(good);
  ASSERT_TRUE(features.ok()) << features.status.detail;
  EXPECT_EQ(store.Release(good).code, StatusCode::FailedPrecondition)
      << "the engine kept a pin on the frame it succeeded with";
  ForgetOutputs(features.value);

  // And the refusal path, which is the one an exception would unwind through.
  FrameRef sheared = Textured();
  sheared.stride = 4;
  EXPECT_FALSE(engine.ExtractFeatures(sheared).ok());
  EXPECT_EQ(store.Release(sheared).code, StatusCode::FailedPrecondition)
      << "the engine kept a pin on the frame it refused";
}

TEST_P(Extraction, ARefusedExtractionLeavesTheStoreExactlyAsItFoundIt) {
  // The rollback, which was reachable and untested: a reviewer deleted the `Forget` that unwinds
  // the descriptor frame and all 640 tests passed, while the deleted line leaked 50,688 bytes per
  // refused SIFT extraction — permanently, since the only handle naming that frame is dropped.
  //
  // The ceiling is set so the input frame and the descriptors fit and the keypoints do not, which
  // is the one ordering that reaches the second allocation's failure.
  const FrameRef frame = Textured();
  const Result<FeatureSet> sized = Engine().ExtractFeatures(frame);
  ASSERT_TRUE(sized.ok()) << sized.status.detail;
  ASSERT_GT(sized.value.count, 0);
  const int64_t descriptorBytes =
      static_cast<int64_t>(sized.value.descriptors.width) * sized.value.descriptors.height;
  const int64_t keypointBytes =
      static_cast<int64_t>(sized.value.keypoints.width) * sized.value.keypoints.height;
  ForgetOutputs(sized.value);

  const int64_t frameBytes = HeapUsed();
  MemoryFrameStoreAccess tight{frameBytes + descriptorBytes + keypointBytes - 1};
  const Result<FrameRef> copied = tight.Allocate(kWidth, kHeight, PixelFormat::RGBA8);
  ASSERT_TRUE(copied.ok()) << copied.status.detail;
  {
    const Result<std::span<uint8_t>> from = store.Pin(frame);
    const Result<std::span<uint8_t>> to = tight.Pin(copied.value);
    ASSERT_TRUE(from.ok() && to.ok());
    ASSERT_EQ(from.value.size(), to.value.size());
    std::memcpy(to.value.data(), from.value.data(), from.value.size());
    EXPECT_TRUE(store.Release(frame).ok());
    EXPECT_TRUE(tight.Release(copied.value).ok());
  }

  const Result<FrameStoreBudget> before = tight.Budget();
  ASSERT_TRUE(before.ok());
  FeatureRegistrationEngine engine{tight, GetParam()};
  const Result<FeatureSet> refused = engine.ExtractFeatures(copied.value);
  ASSERT_FALSE(refused.ok()) << "the ceiling was meant to be one byte short of the second frame";
  const Result<FrameStoreBudget> after = tight.Budget();
  ASSERT_TRUE(after.ok());
  EXPECT_EQ(after.value.heapUsedBytes, before.value.heapUsedBytes)
      << "a refused extraction kept bytes nobody holds a handle to";
}

TEST_P(Extraction, AnswersADegenerateFrameRatherThanLettingOpenCvThrowThroughIt) {
  // The exception boundary ADR 0047 named and ADR 0052 builds. A one-pixel frame passes every guard
  // this engine has — the format is readable, the dimensions are positive, the stride is a full row
  // and the span holds it — and then `cv::ORB` throws `inv_scale_x > 0` out of `resize` and
  // `cv::AKAZE` throws `s >= 0` out of `setSize`. SIFT answers with nothing and does not throw,
  // which is the reason the boundary cannot be a list of the detectors that need it.
  //
  // What is asserted is that a `Result` comes back at all. Without the `catch`, the exception
  // unwinds out of the engine: in this binary gtest reports it as a failure, and in the shipped core
  // it is worse than that, since only this one translation unit is compiled `-fexceptions`.
  FeatureRegistrationEngine engine = Engine();
  const FrameRef onePixel = Textured(1);

  const Result<FeatureSet> features = engine.ExtractFeatures(onePixel);
  if (features.ok()) {
    EXPECT_EQ(features.value.count, 0) << "one pixel has no features in it";
    ForgetOutputs(features.value);
  } else {
    EXPECT_EQ(features.status.code, StatusCode::Internal) << features.status.detail;
    EXPECT_NE(features.status.detail.find("OpenCV"), std::string::npos)
        << "an OpenCV refusal should say so: " << features.status.detail;
  }
  EXPECT_EQ(store.Release(onePixel).code, StatusCode::FailedPrecondition)
      << "the pin was not released on the way out of a throwing call";
}

TEST_P(Extraction, MatchingAndRefinementRefuseRatherThanAnswer) {
  // The two methods this increment does not implement. A reviewer made both return `Ok` — the
  // identity result the header calls dangerous — and the whole file stayed green, so "refuses
  // rather than pretending" was a sentence with nothing behind it.
  FeatureRegistrationEngine engine = Engine();
  const Result<FeatureSet> a = engine.ExtractFeatures(Textured());
  const Result<FeatureSet> b = engine.ExtractFeatures(Textured());
  ASSERT_TRUE(a.ok() && b.ok());

  const Result<PairwiseResult> pair = engine.EstimatePairwise(a.value, b.value, Quat{});
  EXPECT_FALSE(pair.ok()) << "an identity rotation here would look like a registration";
  EXPECT_EQ(pair.status.code, StatusCode::Unsupported);

  const Result<GlobalSolution> refined = engine.Refine({}, {}, Intrinsics{});
  EXPECT_FALSE(refined.ok());
  EXPECT_EQ(refined.status.code, StatusCode::Unsupported);

  ForgetOutputs(a.value);
  ForgetOutputs(b.value);
}

INSTANTIATE_TEST_SUITE_P(EveryDetector, Extraction,
                         ::testing::Values(FeatureDetector::Orb, FeatureDetector::Akaze,
                                           FeatureDetector::Sift),
                         [](const ::testing::TestParamInfo<FeatureDetector>& info) {
                           switch (info.param) {
                             case FeatureDetector::Orb: return "Orb";
                             case FeatureDetector::Akaze: return "Akaze";
                             case FeatureDetector::Sift: return "Sift";
                           }
                           return "Unknown";
                         });

TEST_P(Extraction, TheTexturedFrameHasNoTwoTilesAlike) {
  // The property `Textured()` exists to have, asserted rather than described. Matching arrives in
  // the next increment and a repeating texture would let a wrong correspondence score as a right
  // one — so this is the test that has to be here before that code is, not after it.
  //
  // It reads back the **pixels of the frame `Textured()` paints**, not `TexturedLuma` directly. The
  // first version called the function, which left `Textured()` free to paint something else
  // entirely: a reviewer replaced its body with a plain repeating checkerboard, left `TexturedLuma`
  // untouched, and every test in this file still passed. A test of the ingredient is not a test of
  // the dish.
  const FrameRef frame = Textured();
  const Result<std::span<uint8_t>> pinned = store.Pin(frame);
  ASSERT_TRUE(pinned.ok()) << pinned.status.detail;

  std::vector<std::vector<uint8_t>> tiles;
  for (int32_t ty = 0; ty < kHeight / kTile; ++ty) {
    for (int32_t tx = 0; tx < kWidth / kTile; ++tx) {
      std::vector<uint8_t> tile;
      tile.reserve(static_cast<size_t>(kTile) * kTile);
      for (int32_t j = 0; j < kTile; ++j) {
        for (int32_t i = 0; i < kTile; ++i) {
          const size_t at = static_cast<size_t>(ty * kTile + j) * frame.stride +
                            static_cast<size_t>(tx * kTile + i) * 4;
          tile.push_back(pinned.value[at]);   // the red channel; the painter writes luma to all three
        }
      }
      for (const std::vector<uint8_t>& earlier : tiles) {
        EXPECT_NE(earlier, tile) << "tile (" << tx << ", " << ty << ") repeats an earlier tile";
      }
      tiles.push_back(std::move(tile));
    }
  }
  EXPECT_EQ(tiles.size(), static_cast<size_t>((kWidth / kTile) * (kHeight / kTile)));
  EXPECT_TRUE(store.Release(frame).ok());
}

TEST(NullRegistration, RefusesEverythingRatherThanPretending) {
  // Kept beside the real one so the pair is visible: the null engine is what a WASM build gets
  // (ADR 0052), and it refuses rather than returning an identity that would look like a stitch.
  NullRegistrationEngine engine;
  EXPECT_FALSE(engine.ExtractFeatures(FrameRef{}).ok());
}

}  // namespace
}  // namespace sphanorama

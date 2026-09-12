// V7 — how frames are aligned. This file covers feature extraction and pairwise matching; the
// global refinement arrives in its own increment.
//
// It said "feature extraction only" until a reviewer read the whole file rather than the diff. The
// branch that added `EstimatePairwise` added 538 lines of matching tests below and never touched
// these two, because a header at line 1 is outside every range diff — which is the shape CLAUDE.md
// records from PR #49's fourteenth round. The first thing a reader met was a sentence telling them
// the tests they came for were somewhere else.
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
#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numbers>
#include <utility>
#include <vector>

#include "engines/registration_engine/feature_registration_engine.h"
#include "utilities/quaternion.h"
#include "engines/registration_engine/null_registration_engine.h"
#include "resource_access/frame_store_access/memory_frame_store_access.h"
#include "utilities/pixel_format.h"
#include "support/fake_spill_sink.h"

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

/**
 * A frame store that pads every row it allocates, which the contract permits and ours never does.
 *
 * It exists to reach the one rollback path that runs with both output frames *pinned*: the packing
 * guard fires after `Pin`, so `OwnedFrame`'s destructor has to release before it forgets. Nothing
 * else in the suite gets there — the reachable refusals all happen before the pins are taken — and
 * a reviewer showed that swapping those two lines leaves every test in the repository green while
 * orphaning the bytes of every refusal that holds a pin.
 */
class PaddingFrameStore : public IFrameStoreAccess {
 public:
  /** How this store lies about what it allocated. */
  enum class Lie {
    PadRows,     ///< allocate wider than asked and report the asked-for width: stride != width
    ShortRows,   ///< allocate *fewer* rows than asked and report the asked-for height
  };

  explicit PaddingFrameStore(int64_t ceiling, Lie lie = Lie::PadRows)
      : inner_(ceiling), lie_(lie) {}

  Result<FrameRef> Allocate(int32_t width, int32_t height, PixelFormat format) override {
    // Two different lies, because the engine's store check has two halves and `PadRows` reaches
    // only one of them. A store that pads rows keeps the total large enough for any size check to
    // pass; one that drops a row keeps `stride == width` and fails only the size check.
    //
    // **Only the engine's own allocations are lied to.** The descriptor and keypoint frames are
    // `Gray8`; the frame a test paints is not. Lying about both made the short-rows case refuse at
    // the *input* geometry guard instead — the test passed, asserting the wrong status code, for
    // the wrong reason, and neutering the check it was written for changed nothing. Found by
    // sabotaging it.
    const bool engineOwn = format == PixelFormat::Gray8;
    const int32_t realWidth = (engineOwn && lie_ == Lie::PadRows) ? width + kPad : width;
    const int32_t realHeight =
        (engineOwn && lie_ == Lie::ShortRows) ? std::max(1, height - 1) : height;
    Result<FrameRef> allocated = inner_.Allocate(realWidth, realHeight, format);
    if (!allocated.ok()) return allocated;
    padded_.push_back(allocated.value);
    FrameRef reported = allocated.value;
    reported.width = width;
    reported.height = height;
    if (engineOwn && lie_ == Lie::ShortRows) {
      reported.stride = width * std::max(BytesPerPixel(format), 1);
    }
    return Ok(reported);
  }

  Result<std::span<uint8_t>> Pin(const FrameRef& frame) override { return inner_.Pin(Real(frame)); }
  Status Release(const FrameRef& frame) override { return inner_.Release(Real(frame)); }
  Status Forget(const FrameRef& frame) override { return inner_.Forget(Real(frame)); }
  Result<FrameStoreBudget> Budget() override { return inner_.Budget(); }
  Result<Residency> ResidencyOf(const FrameRef& f) override { return inner_.ResidencyOf(Real(f)); }
  Status Demote(const FrameRef& f, Residency t) override { return inner_.Demote(Real(f), t); }
  Status Adopt(const FrameRef& f) override { return inner_.Adopt(Real(f)); }
  Status Clear() override { return inner_.Clear(); }
  Result<uint64_t> TierGeneration() override { return inner_.TierGeneration(); }
  Result<uint64_t> ContentHash(const FrameRef& f) override { return inner_.ContentHash(Real(f)); }

 private:
  static constexpr int32_t kPad = 8;

  /** The entry as the inner store knows it — the padded one, keyed by id. */
  FrameRef Real(const FrameRef& frame) const {
    for (const FrameRef& padded : padded_) {
      if (padded.id.value == frame.id.value) return padded;
    }
    return frame;
  }

  MemoryFrameStoreAccess inner_;
  Lie lie_;
  std::vector<FrameRef> padded_;
};

class Extraction : public ::testing::TestWithParam<FeatureDetector> {
 protected:
  MemoryFrameStoreAccess store{1 << 24};

  FeatureRegistrationEngine Engine() { return FeatureRegistrationEngine{store, GetParam()}; }

  /**
   * The lens the textured frames are taken to have been seen through — square, centred, undistorted.
   *
   * Undistorted on purpose: these tests are about matching and the rotation fit, and a Brown-Conrady
   * term here would mean a failure could be either. The distortion path is `camera_model`'s own
   * tests' subject, and ADR 0054 records that a wrong lens makes this engine wrong quietly.
   */
  static Intrinsics Lens(int32_t edge = kWidth) {
    Intrinsics lens{};
    lens.fx = lens.fy = static_cast<double>(edge);
    lens.cx = lens.cy = static_cast<double>(edge) / 2.0;
    lens.width = lens.height = edge;
    return lens;
  }

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
    // Given back on every path out, including the `ASSERT_*` below, which returns from this helper
    // and not from the caller. An earlier version wrote that cleanup as an `if` — correct, and
    // unreachable: `Pin` cannot fail for a frame this line just allocated in this store, so nothing
    // ever entered it. A destructor needs no such branch and is right whether or not one exists.
    struct GiveBack {
      IFrameStoreAccess& frames;
      FrameRef frame;
      bool keep = false;
      ~GiveBack() {
        if (keep) return;
        (void)frames.Forget(frame);
      }
    } giveBack{store, frame};
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
    giveBack.keep = true;
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
    // `Paint` is void, so its `ASSERT_*` failures return from *it* and not from the caller: without
    // this the caller would go on to extract from a default-constructed handle and assert against
    // whatever that produced, reporting a second, invented failure on top of the real one.
    EXPECT_NE(frame.id.value, 0U) << "the painter did not produce a frame";
    return frame;
  }

  /**
   * A frame ruled into eight-pixel squares — the fixture that makes a detector overrun its cap.
   *
   * `Textured` does not: asked for 500 features at 768 square it gets 500 from ORB, 500 from AKAZE
   * and 501 from SIFT, so the engine's truncation dropped exactly one row in the whole suite. On
   * this pattern ORB answers 1,145 and SIFT 740, both measured on the pinned OpenCV. AKAZE still
   * answers exactly 500, which is why the test below names it as the detector that does not overrun
   * rather than asserting an overrun it would fail.
   */
  FrameRef Ruled(int32_t edge) {
    FrameRef frame;
    Paint(&frame, [](int32_t x, int32_t y) -> uint8_t {
      return (x % 8 == 0 || y % 8 == 0) ? 255 : 0;
    }, edge);
    EXPECT_NE(frame.id.value, 0U) << "the painter did not produce a frame";
    return frame;
  }

  /**
   * The same detector, made independently of the engine's own `Make()` — including its cap.
   *
   * The cap has to be here too, and an earlier version left it out on the reasoning that the engine
   * truncates afterwards so the oracle's extra features could just be trimmed. That is false: a
   * detector *asked* for 50 features does not return the first 50 an uncapped one returns.
   * `retainBest` selects by response across the whole set, so the two lists differ from row 0. The
   * comparison was inert only because no frame in this file reaches the cap; lowering
   * `kMaxFeaturesPerFrame` to 50 failed all three rows while the engine was entirely correct.
   *
   * The cap comes from the engine's header rather than being restated here, so tuning it moves both
   * sides at once. What stays independent is everything this oracle is for: the detector mapping,
   * the ordering and the coordinates — an oracle sharing `Make()` would agree with a wrong mapping.
   */
  static cv::Ptr<cv::Feature2D> OpenCvDetector(FeatureDetector detector) {
    switch (detector) {
      case FeatureDetector::Orb:   return cv::ORB::create(kMaxFeaturesPerFrame);
      case FeatureDetector::Sift:  return cv::SIFT::create(kMaxFeaturesPerFrame);
      case FeatureDetector::Akaze: {
        // OpenCV's defaults taken *from OpenCV*, with the one value this project overrides set
        // afterwards. This used to restate all seven, which made the oracle a second copy of the
        // engine's list rather than an independent check of it: a reviewer changed `nOctaveLayers`
        // in `Make()` and here together, and every test passed while AKAZE ran with a number nobody
        // chose. Now only `Make()` restates them, so that drift fails the row comparisons.
        cv::Ptr<cv::AKAZE> akaze = cv::AKAZE::create();
        akaze->setMaxPoints(kMaxFeaturesPerFrame);
        return akaze;
      }
      case FeatureDetector::Count:
        break;   // not a detector; the engine refuses it and so does this oracle
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
  ForgetOutputs(features.value);
}

TEST_P(Extraction, FindsNothingOnAFlatFrame) {
  // The invariant that separates "found features" from "returned a number". A frame with no
  // gradient anywhere has nothing to find, and a detector reporting features on it is reporting
  // noise — which would then be matched against noise in the next frame.
  FeatureRegistrationEngine engine = Engine();
  const Result<FeatureSet> features = engine.ExtractFeatures(Blank());

  ASSERT_TRUE(features.ok()) << features.status.detail;
  EXPECT_EQ(features.value.count, 0);
  // `count == 0` promises there is nothing to forget, and `ForgetOutputs` branches on that promise.
  // Unasserted, an engine that allocated two empty frames and reported zero would leak them
  // silently on every flat frame in a capture.
  EXPECT_EQ(features.value.descriptors.id.value, 0U) << "count == 0 must mean no frame was taken";
  EXPECT_EQ(features.value.keypoints.id.value, 0U);
  EXPECT_EQ(store.Forget(features.value.descriptors).code, StatusCode::NotFound);
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
  EXPECT_TRUE(store.Release(first.value.descriptors).ok());
  EXPECT_TRUE(store.Release(second.value.descriptors).ok());
  ForgetOutputs(first.value);
  ForgetOutputs(second.value);
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
  EXPECT_TRUE(store.Release(descriptors).ok());
  ForgetOutputs(features.value);
}

TEST_P(Extraction, EachDescriptorStaysWithItsKeypoint) {
  // The pairing, which nothing checked: gathering descriptor rows by their position instead of
  // through the same permutation as the keypoints left every test in the repository green. A
  // descriptor filed against the wrong keypoint is not a crash and not a leak — it is a matcher
  // that confidently corresponds the wrong features, which is the one failure this whole engine
  // exists to avoid and the hardest to see afterwards.
  //
  // Checked against the detector run independently, ordered the way `FeatureSet` promises, so both
  // halves of each row are compared: the coordinates *and* the descriptor bytes that must belong
  // to them.
  FeatureRegistrationEngine engine = Engine();
  const FrameRef source = Textured();
  const Result<FeatureSet> features = engine.ExtractFeatures(source);
  ASSERT_TRUE(features.ok()) << features.status.detail;
  ASSERT_GT(features.value.count, 0);

  const cv::Ptr<cv::Feature2D> oracle = OpenCvDetector(GetParam());
  ASSERT_TRUE(oracle);
  const Result<std::span<uint8_t>> sourceBytes = store.Pin(source);
  ASSERT_TRUE(sourceBytes.ok()) << sourceBytes.status.detail;
  const cv::Mat colour(kHeight, kWidth, CV_8UC4, sourceBytes.value.data(), source.stride);
  cv::Mat grey;
  cv::cvtColor(colour, grey, cv::COLOR_RGBA2GRAY);
  std::vector<cv::KeyPoint> expected;
  cv::Mat expectedDescriptors;
  oracle->detectAndCompute(grey, cv::noArray(), expected, expectedDescriptors);
  EXPECT_TRUE(store.Release(source).ok());

  std::vector<int> order(expected.size());
  for (size_t at = 0; at < order.size(); ++at) order[at] = static_cast<int>(at);
  std::stable_sort(order.begin(), order.end(), [&expected](int a, int b) {
    return expected[static_cast<size_t>(a)].response > expected[static_cast<size_t>(b)].response;
  });
  ASSERT_GE(static_cast<int32_t>(order.size()), features.value.count);

  const Result<std::span<uint8_t>> keypointBytes = store.Pin(features.value.keypoints);
  const Result<std::span<uint8_t>> descriptorBytes = store.Pin(features.value.descriptors);
  ASSERT_TRUE(keypointBytes.ok() && descriptorBytes.ok());
  const size_t width = static_cast<size_t>(features.value.descriptors.width);

  for (int32_t row = 0; row < features.value.count; ++row) {
    const int from = order[static_cast<size_t>(row)];
    float xy[2] = {0.0F, 0.0F};
    std::memcpy(xy, keypointBytes.value.data() + static_cast<size_t>(row) * 8, sizeof(xy));
    ASSERT_FLOAT_EQ(xy[0], expected[static_cast<size_t>(from)].pt.x) << "row " << row;
    ASSERT_FLOAT_EQ(xy[1], expected[static_cast<size_t>(from)].pt.y) << "row " << row;
    EXPECT_EQ(0, std::memcmp(descriptorBytes.value.data() + static_cast<size_t>(row) * width,
                             expectedDescriptors.ptr(from), width))
        << "row " << row << ": this descriptor belongs to a different keypoint";
  }

  EXPECT_TRUE(store.Release(features.value.keypoints).ok());
  EXPECT_TRUE(store.Release(features.value.descriptors).ok());
  ForgetOutputs(features.value);
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

TEST_P(Extraction, RefusesAHandleWhoseOwnArithmeticWouldOverflow) {
  // The guard added last round widened `width * bytesPerPixel` into int64 and then multiplied again
  // without asking. `(height - 1) * stride` overflows when `stride` falls back to `rowBytes`: with
  // `stride <= 0` the fallback is `width * 4`, which is bounded by 2^33 rather than by `int32_t`,
  // so a claim of INT32_MAX by INT32_MAX reaches ~1.8e19 and wraps **negative** — and a negative
  // `needed` is less than any size, so the guard passes the handle it was written to refuse.
  //
  // Under the sanitizer preset it is `-fno-sanitize-recover=all` on a signed overflow, so the job
  // aborts; without sanitizers it is a silent bypass into a `cv::Mat` over 16 KB. What stops it
  // being an out-of-bounds read today is an accident — the dimensions are so large that OpenCV's
  // allocator throws first, into the `catch` this PR happens to have added.
  FeatureRegistrationEngine engine = Engine();
  FrameRef absurd = Textured();
  absurd.format = PixelFormat::RGBA8;
  absurd.width = std::numeric_limits<int32_t>::max();
  absurd.height = std::numeric_limits<int32_t>::max();
  absurd.stride = 0;   // forces the `: rowBytes` half of the ternary, which is the unbounded one

  const Result<FeatureSet> features = engine.ExtractFeatures(absurd);
  ASSERT_FALSE(features.ok());
  EXPECT_EQ(features.status.code, StatusCode::InvalidArgument) << features.status.detail;
}

TEST_P(Extraction, RefusesAPlanarHandleClaimingChromaIsPicture) {
  // For NV12 and I420 the pinned buffer holds chroma after the luma plane, so bounding the claim by
  // the *bytes pinned* is not the same as bounding it by the *picture*. A real I420 128x128 is
  // 24,576 bytes; a handle claiming 192 rows needs exactly 24,576 by the luma arithmetic and is
  // accepted, so 64 rows of chroma get detected as picture — in bounds, so ASan stays silent.
  const Result<FrameRef> allocated = store.Allocate(kWidth, kHeight, PixelFormat::I420);
  ASSERT_TRUE(allocated.ok()) << allocated.status.detail;
  FrameRef overclaimed = allocated.value;
  overclaimed.height = kHeight + kHeight / 2;

  const Result<FeatureSet> features = Engine().ExtractFeatures(overclaimed);
  ASSERT_FALSE(features.ok());
  EXPECT_EQ(features.status.code, StatusCode::InvalidArgument) << features.status.detail;
}

TEST_P(Extraction, RefusesAPlanarHandleThatHidesChromaBehindAWideStride) {
  // The planar check bounds `FrameByteSize(width, height, format)`, which is *packed* bytes, while
  // the luma check above it is in *strided* bytes. A claim whose stride exceeds its width satisfies
  // both at once, so narrowing the claimed width buys enough packed budget to pay for extra rows.
  //
  // Against a real packed I420 640x480 (460,800 bytes, picture ending at 307,200) the handle
  // {width 400, height 720, stride 640} passes every check — `FrameByteSize(400, 720, I420)` is
  // 432,000 — and a detector then reads 240 rows of chroma as picture. In bounds, so ASan is silent.
  //
  // Refusing a planar frame whose rows are not packed is what closes it, and it refuses nothing
  // real: `MemoryFrameStoreAccess::Allocate` is the only line in the repository that ever sets a
  // stride, and it packs planar rows.
  const Result<FrameRef> allocated = store.Allocate(640, 480, PixelFormat::I420);
  ASSERT_TRUE(allocated.ok()) << allocated.status.detail;
  FrameRef smuggled = allocated.value;
  smuggled.width = 400;
  smuggled.height = 720;
  smuggled.stride = 640;

  const Result<FeatureSet> features = Engine().ExtractFeatures(smuggled);
  ASSERT_FALSE(features.ok()) << "chroma was accepted as picture";
  EXPECT_EQ(features.status.code, StatusCode::InvalidArgument) << features.status.detail;
  EXPECT_TRUE(store.Forget(allocated.value).ok());
}

TEST_P(Extraction, RefusesAFormatWithNoLumaPlaneToRead) {
  // The contract promises `Unsupported` for "a format with no luma plane to read", and nothing
  // asked. Found by sabotage rather than by reading: making the engine's format list claim an
  // *encoded* frame carries a planar luma passed all 708 tests, because every other case in this
  // file hands in a format that does have one. The opposite direction was covered six times over.
  //
  // `EncodedJpeg` is the real instance — a frame straight off a camera that has not been decoded —
  // and reading its bytes as a luma plane would score compressed data as picture.
  // **Against a cold frame, because that is the only arrangement that can tell where the guard
  // is.** The first version allocated a resident frame and asserted its residency was still
  // `HeapEncoded` afterwards — which the `Unpin` holder delivers whether the format is checked
  // before the pin or after it, so a reviewer moved `HasReadableLuma` to below the pin and all 714
  // tests stayed green. A spilled frame separates them: `Pin` faults one back into the heap and
  // leaves it there (`ReadingASpilledFrameLeavesItInTheHeap` is the proof), so a guard that ran
  // after the pin would un-cool a frame `CaptureSessionManager` deliberately cooled — on a call
  // whose whole job here is to refuse for free.
  FakeSpillSink sink;
  MemoryFrameStoreAccess spilling{1 << 24, &sink};
  const Result<FrameRef> allocated = spilling.Allocate(kWidth, kHeight, PixelFormat::RGBA8);
  ASSERT_TRUE(allocated.ok()) << allocated.status.detail;
  ASSERT_TRUE(spilling.Demote(allocated.value, Residency::Spilled).ok());
  const Result<Residency> cold = spilling.ResidencyOf(allocated.value);
  ASSERT_TRUE(cold.ok());
  ASSERT_EQ(cold.value, Residency::Spilled) << "the fixture did not manage to cool the frame";

  FrameRef encoded = allocated.value;
  encoded.format = PixelFormat::EncodedJpeg;

  FeatureRegistrationEngine engine{spilling, GetParam()};
  const Result<FeatureSet> features = engine.ExtractFeatures(encoded);
  EXPECT_FALSE(features.ok()) << "an encoded frame's bytes are not a picture";
  EXPECT_EQ(features.status.code, StatusCode::Unsupported);

  const Result<Residency> after = spilling.ResidencyOf(allocated.value);
  ASSERT_TRUE(after.ok());
  EXPECT_EQ(after.value, Residency::Spilled)
      << "the refusal faulted the frame in, so the format guard is running after the pin rather "
         "than before it";
  EXPECT_TRUE(spilling.Forget(allocated.value).ok());
}

TEST_P(Extraction, RefusesAFrameWithNoPixelsInIt) {
  // The first line of the guard pair, and the one nothing asked about — removing `width <= 0 ||
  // height <= 0` from *both* engines leaves every test in the repository green. What it buys is the
  // difference between a refusal a caller can branch on and OpenCV's assertion text arriving as
  // `Internal`: without it, a zero or negative dimension reaches `cv::Mat` and comes back as a
  // converted exception saying nothing a caller could act on.
  FeatureRegistrationEngine engine = Engine();
  const FrameRef source = Textured();
  for (const auto& [width, height] : std::vector<std::pair<int32_t, int32_t>>{
           {0, kHeight}, {kWidth, 0}, {-1, kHeight}, {kWidth, -1}, {-1000000, kHeight}}) {
    FrameRef empty = source;
    empty.width = width;
    empty.height = height;
    const Result<FeatureSet> features = engine.ExtractFeatures(empty);
    ASSERT_FALSE(features.ok()) << width << "x" << height;
    EXPECT_EQ(features.status.code, StatusCode::InvalidArgument)
        << width << "x" << height << ": " << features.status.detail;
  }
}

TEST_P(Extraction, RefusesOneEnormousRowTheStoreIsNotHolding) {
  // The `else` branch of the geometry bound, which round 3 claimed was load-bearing and no test
  // held. A frame of exactly one row skips the division — there is no product to bound — so this is
  // the only thing between a single enormous row and the span it would be read from. Deleting it
  // leaves every test in the repository green and gives ASan `heap-buffer-overflow READ of size
  // 400000` out of a **16-byte** region — the frame allocated two lines below — inside ORB's
  // `copyMakeBorder_8u`. An earlier draft said 64, which was not this test's allocation.
  const Result<FrameRef> allocated = store.Allocate(16, 1, PixelFormat::Gray8);
  ASSERT_TRUE(allocated.ok()) << allocated.status.detail;
  FrameRef oneHugeRow = allocated.value;
  oneHugeRow.width = 400000;
  oneHugeRow.height = 1;
  oneHugeRow.stride = 0;   // no stride to fall back on, so the row itself is the whole claim

  const Result<FeatureSet> features = Engine().ExtractFeatures(oneHugeRow);
  ASSERT_FALSE(features.ok());
  EXPECT_EQ(features.status.code, StatusCode::InvalidArgument) << features.status.detail;
  EXPECT_TRUE(store.Forget(allocated.value).ok());
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

  // **Row for row against OpenCV's own answer**, not a count of how many rows differ. A reviewer
  // rewrote these rows in reverse order and the previous version stayed green; so did swapping `x`
  // with `y`, because the fixture frame is square and the bounds symmetric. Both are exactly the
  // defects that would make matching correspond the wrong features, and both are invisible to any
  // assertion that only asks whether the numbers look like coordinates.
  //
  // The reference runs the same detector over the same luma independently of the engine, which is
  // the same trick the descriptor-width assertion uses. It pins the ordering the comment claims
  // ("x then y") rather than stating it in a string nothing reads.
  const cv::Ptr<cv::Feature2D> oracle = OpenCvDetector(GetParam());
  ASSERT_TRUE(oracle);
  const Result<std::span<uint8_t>> sourceBytes = store.Pin(source);
  ASSERT_TRUE(sourceBytes.ok()) << sourceBytes.status.detail;
  const cv::Mat colour(kHeight, kWidth, CV_8UC4, sourceBytes.value.data(), source.stride);
  cv::Mat grey;
  cv::cvtColor(colour, grey, cv::COLOR_RGBA2GRAY);
  std::vector<cv::KeyPoint> expected;
  cv::Mat ignored;
  oracle->detectAndCompute(grey, cv::noArray(), expected, ignored);
  EXPECT_TRUE(store.Release(source).ok());
  // Ordered and truncated the way `FeatureSet` says its rows are — best-first, ties in the order the
  // detector found them — because that is the promise being checked. Comparing against the
  // detector's raw order would assume the engine hands rows back untouched, which it deliberately
  // does not: two of the three detectors return an unsorted list.
  std::stable_sort(expected.begin(), expected.end(),
                   [](const cv::KeyPoint& a, const cv::KeyPoint& b) { return a.response > b.response; });
  if (expected.size() > static_cast<size_t>(features.value.count)) {
    expected.resize(static_cast<size_t>(features.value.count));
  }
  ASSERT_EQ(static_cast<int32_t>(expected.size()), features.value.count);

  for (int32_t row = 0; row < features.value.count; ++row) {
    float xy[2] = {0.0F, 0.0F};
    std::memcpy(xy, pinned.value.data() + static_cast<size_t>(row) * 8, sizeof(xy));
    EXPECT_FLOAT_EQ(xy[0], expected[row].pt.x) << "row " << row << ": x is not this row's x";
    EXPECT_FLOAT_EQ(xy[1], expected[row].pt.y) << "row " << row << ": y is not this row's y";
  }

  EXPECT_TRUE(store.Release(keypoints).ok());
  ForgetOutputs(features.value);
}

TEST_P(Extraction, TheCapIsAskedForAndNotOnlyTruncatedTo) {
  // `EXPECT_LE(count, cap)` cannot tell a detector that was *asked* for the cap from one that
  // returned everything and got cut down afterwards, because the truncation satisfies it either
  // way — and the row-for-row comparison runs at 128 square, where no detector reaches the cap at
  // all. Dropping the cap from `Make()` for SIFT and AKAZE left all 690 tests green.
  //
  // At 768 the cap binds, and an uncapped detector is not a prefix of a capped one: `retainBest`
  // selects across the whole set, so the lists differ from row 0. Measured with the cap dropped:
  // 500 of 500 AKAZE rows and 424 of 500 SIFT rows disagree with the capped oracle. With it, none.
  //
  // **The ORB row cannot catch a dropped cap and is kept anyway.** `cv::ORB::create()`'s default
  // `nfeatures` is 500, which is what `kMaxFeaturesPerFrame` happens to be, so removing the cap from
  // ORB's line in `Make()` is a behavioural no-op. What the row does catch is a *wrong* cap — 1,000
  // there does fail it — and it is the row that will start meaning something the day OpenCV changes
  // that default or this project changes its number. Saying so is better than letting a reader
  // count three live rows where there are two.
  FeatureRegistrationEngine engine = Engine();
  const FrameRef source = Textured(768);
  const Result<FeatureSet> features = engine.ExtractFeatures(source);
  ASSERT_TRUE(features.ok()) << features.status.detail;
  ASSERT_EQ(features.value.count, kMaxFeaturesPerFrame)
      << "this frame is meant to be big enough that the cap binds; if it no longer does, this test "
         "has stopped asking anything and the size is what to fix";

  const cv::Ptr<cv::Feature2D> oracle = OpenCvDetector(GetParam());
  ASSERT_TRUE(oracle);
  const Result<std::span<uint8_t>> sourceBytes = store.Pin(source);
  ASSERT_TRUE(sourceBytes.ok()) << sourceBytes.status.detail;
  const cv::Mat colour(768, 768, CV_8UC4, sourceBytes.value.data(), source.stride);
  cv::Mat grey;
  cv::cvtColor(colour, grey, cv::COLOR_RGBA2GRAY);
  std::vector<cv::KeyPoint> expected;
  cv::Mat ignored;
  oracle->detectAndCompute(grey, cv::noArray(), expected, ignored);
  EXPECT_TRUE(store.Release(source).ok());
  std::stable_sort(expected.begin(), expected.end(),
                   [](const cv::KeyPoint& a, const cv::KeyPoint& b) { return a.response > b.response; });
  if (expected.size() > static_cast<size_t>(features.value.count)) {
    expected.resize(static_cast<size_t>(features.value.count));
  }
  ASSERT_EQ(static_cast<int32_t>(expected.size()), features.value.count);

  const Result<std::span<uint8_t>> pinned = store.Pin(features.value.keypoints);
  ASSERT_TRUE(pinned.ok()) << pinned.status.detail;
  int32_t disagreements = 0;
  for (int32_t row = 0; row < features.value.count; ++row) {
    float xy[2] = {0.0F, 0.0F};
    std::memcpy(xy, pinned.value.data() + static_cast<size_t>(row) * 8, sizeof(xy));
    if (xy[0] != expected[row].pt.x || xy[1] != expected[row].pt.y) ++disagreements;
  }
  EXPECT_EQ(disagreements, 0)
      << disagreements << " of " << features.value.count
      << " rows differ from a detector built with the same cap — this engine's detector was not";
  EXPECT_TRUE(store.Release(features.value.keypoints).ok());
  ForgetOutputs(features.value);
}

TEST_P(Extraction, KeepsTheBestRowsWhenTheDetectorOverrunsTheCapOutright) {
  // The line that makes the cap true when a detector ignores it had never been asked to do anything.
  // Across the whole suite the truncation dropped exactly one row — SIFT's 501 for a request of 500
  // on `Textured` at 768 — and the round before this one widened the code feeding it. `Ruled`
  // overruns properly: ORB answers 1,145 there and SIFT 740.
  //
  // The count alone is not the assertion, because truncating to *any* 500 rows satisfies it. What is
  // asserted is that the 500 kept are the 500 strongest of the list the detector actually returned,
  // which is the difference between keeping the best and keeping the first. On ORB that difference
  // is real rather than theoretical: `orb.cpp` caps each pyramid level separately and concatenates
  // them, so the first 500 of its 1,145 are whole octaves rather than the strongest responses.
  //
  // **Only the ORB row catches it**, which is worth saying rather than implying. Truncating before
  // sorting fails `/Orb` alone: AKAZE is exempt above because it answers exactly the cap here, and
  // SIFT passes because its `retainBest` list is already response-ordered on this fixture, so
  // cutting it first removes the same rows either way. Two of the three rows are covering the cap,
  // not the order.
  FeatureRegistrationEngine engine = Engine();
  const FrameRef source = Ruled(768);
  const Result<FeatureSet> features = engine.ExtractFeatures(source);
  ASSERT_TRUE(features.ok()) << features.status.detail;
  ASSERT_EQ(features.value.count, kMaxFeaturesPerFrame);

  const cv::Ptr<cv::Feature2D> oracle = OpenCvDetector(GetParam());
  ASSERT_TRUE(oracle);
  const Result<std::span<uint8_t>> sourceBytes = store.Pin(source);
  ASSERT_TRUE(sourceBytes.ok()) << sourceBytes.status.detail;
  const cv::Mat colour(768, 768, CV_8UC4, sourceBytes.value.data(), source.stride);
  cv::Mat grey;
  cv::cvtColor(colour, grey, cv::COLOR_RGBA2GRAY);
  std::vector<cv::KeyPoint> expected;
  cv::Mat ignored;
  oracle->detectAndCompute(grey, cv::noArray(), expected, ignored);
  EXPECT_TRUE(store.Release(source).ok());

  // The precondition, asserted rather than assumed: if OpenCV's counts move and this frame stops
  // overrunning the cap, the test has quietly stopped exercising the truncation and the fixture is
  // what to fix. AKAZE is exempt because it answers exactly the cap here — named, not skipped.
  if (GetParam() != FeatureDetector::Akaze) {
    ASSERT_GT(static_cast<int32_t>(expected.size()), kMaxFeaturesPerFrame)
        << "this fixture exists to overrun the cap and no longer does";
  }

  // Stable, because `FeatureSet` promises ties come back in the order the detector found them. A
  // plain sort would be just as deterministic and would break that promise wherever responses tie —
  // and on this fixture they tie in quantity, which is what makes this comparison pin stability
  // rather than merely repeatability.
  std::stable_sort(expected.begin(), expected.end(),
                   [](const cv::KeyPoint& a, const cv::KeyPoint& b) { return a.response > b.response; });
  expected.resize(static_cast<size_t>(kMaxFeaturesPerFrame));

  const Result<std::span<uint8_t>> pinned = store.Pin(features.value.keypoints);
  ASSERT_TRUE(pinned.ok()) << pinned.status.detail;
  int32_t disagreements = 0;
  for (int32_t row = 0; row < features.value.count; ++row) {
    float xy[2] = {0.0F, 0.0F};
    std::memcpy(xy, pinned.value.data() + static_cast<size_t>(row) * 8, sizeof(xy));
    if (xy[0] != expected[row].pt.x || xy[1] != expected[row].pt.y) ++disagreements;
  }
  EXPECT_EQ(disagreements, 0)
      << disagreements << " of " << features.value.count
      << " rows are not the strongest the detector returned — the cap kept the first rows, not the "
         "best ones";
  EXPECT_TRUE(store.Release(features.value.keypoints).ok());
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
  EXPECT_LE(features.value.count, kMaxFeaturesPerFrame);
  // For every detector, including ORB. The `if (GetParam() != Orb)` this replaces removed the one
  // assertion the ORB row could ever fail — with the cap as its own `nfeatures`, passing 0 by
  // mistake means *zero features* to OpenCV's ORB rather than unlimited, and only this catches it.
  // The upper bound above is the trivial half for ORB; this is the half that is not.
  EXPECT_GT(features.value.count, 0) << "a frame this size has features; a zero here means the cap "
                                        "was applied as a floor rather than a ceiling";
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

  // And a refusal, which unwinds through the same holder. This one is refused by the engine's own
  // stride guard *before* OpenCV is entered — an earlier comment here called it "the path an
  // exception would unwind through", which it is not, and the degenerate-frame test below is the
  // one that actually throws. Both paths are worth holding: a guard's `return` and a throw unwind
  // the same destructor, but only one of them existed when it was written.
  FrameRef sheared = Textured();
  sheared.stride = 4;
  EXPECT_FALSE(engine.ExtractFeatures(sheared).ok());
  EXPECT_EQ(store.Release(sheared).code, StatusCode::FailedPrecondition)
      << "the engine kept a pin on the frame it refused";
}

TEST_P(Extraction, ARefusedExtractionGivesBackEveryByteItTook) {
  // The rollback, which was reachable and untested: a reviewer deleted the `Forget` that unwinds
  // the descriptor frame and all 640 tests passed, while the deleted line leaked 50,688 bytes per
  // refused SIFT extraction — permanently, since the only handle naming that frame is dropped.
  //
  // This is about the bytes the *engine* took, which is why it is no longer called "leaves the
  // store exactly as it found it". It does not: reading a frame pins it, and the test below is the
  // case where that changes something a caller can see.
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

  // Per detector, because the behaviour is per detector and measured. An `if (ok()) … else …` that
  // accepted either answer could not tell a *converted* throw from a *swallowed* one — a reviewer
  // replaced the handler with one returning `Ok` and `count == 0`, which for registration is the
  // opposite answer, and the whole file stayed green.
  if (GetParam() == FeatureDetector::Sift) {
    // SIFT does not throw here; it finds nothing. That is why the boundary cannot be a list of the
    // detectors that need one, and why this row is not evidence about the `catch` either way.
    ASSERT_TRUE(features.ok()) << features.status.detail;
    EXPECT_EQ(features.value.count, 0) << "one pixel has no features in it";
    ForgetOutputs(features.value);
  } else {
    // ORB throws `inv_scale_x > 0` out of `resize`, AKAZE `s >= 0` out of `setSize`. A refusal
    // carrying OpenCV's own text is the proof that the throw was converted rather than absorbed.
    ASSERT_FALSE(features.ok()) << "a throw was swallowed into a successful empty answer";
    EXPECT_EQ(features.status.code, StatusCode::Internal) << features.status.detail;
    EXPECT_NE(features.status.detail.find("OpenCV"), std::string::npos)
        << "a converted OpenCV exception should carry its message: " << features.status.detail;
  }
  EXPECT_EQ(store.Release(onePixel).code, StatusCode::FailedPrecondition)
      << "the pin was not released on the way out of a throwing call";
}

TEST_P(Extraction, RefinementRefusesRatherThanAnswering) {
  // The method this increment still does not implement. A reviewer made it return `Ok` — the empty
  // solution the header calls dangerous — and the whole file stayed green, so "refuses rather than
  // pretending" was a sentence with nothing behind it.
  //
  // **`EstimatePairwise` used to be asserted here too, and now is not**, because it answers. Its
  // refusals have their own tests below; what this one holds is the `Refine` half, and the pairing
  // was an accident of both being unimplemented at once rather than a property they share.
  FeatureRegistrationEngine engine = Engine();
  const Result<FeatureSet> a = engine.ExtractFeatures(Textured());
  const Result<FeatureSet> b = engine.ExtractFeatures(Textured());
  ASSERT_TRUE(a.ok() && b.ok());

  const Result<GlobalSolution> refined = engine.Refine({}, {}, Intrinsics{});
  EXPECT_FALSE(refined.ok());
  EXPECT_EQ(refined.status.code, StatusCode::Unsupported);

  ForgetOutputs(a.value);
  ForgetOutputs(b.value);
}

/**
 * A store that counts what was forgotten, so "this call forgets none of the four" is measurable.
 *
 * The contract is emphatic about this and says why: the shape invites the opposite, because
 * `EstimatePairwise` is the method with the obvious-looking reason to release what it was handed.
 * A caller may estimate the same pair twice, or one set against several others, so an
 * implementation that tidied up after itself would destroy the second call's input — and the
 * damage would show up as a `NotFound` somewhere else entirely.
 */
class CountingForgets final : public IFrameStoreAccess {
 public:
  explicit CountingForgets(IFrameStoreAccess& inner) : inner_(inner) {}

  int forgets = 0;

  Status Forget(const FrameRef& f) override {
    ++forgets;
    return inner_.Forget(f);
  }

  Result<FrameRef> Allocate(int32_t w, int32_t h, PixelFormat f) override {
    return inner_.Allocate(w, h, f);
  }
  Result<std::span<uint8_t>> Pin(const FrameRef& f) override { return inner_.Pin(f); }
  Status Release(const FrameRef& f) override { return inner_.Release(f); }
  Result<FrameStoreBudget> Budget() override { return inner_.Budget(); }
  Result<Residency> ResidencyOf(const FrameRef& f) override { return inner_.ResidencyOf(f); }
  Status Demote(const FrameRef& f, Residency t) override { return inner_.Demote(f, t); }
  Status Adopt(const FrameRef& f) override { return inner_.Adopt(f); }
  Status Clear() override { return inner_.Clear(); }
  Result<uint64_t> TierGeneration() override { return inner_.TierGeneration(); }
  Result<uint64_t> ContentHash(const FrameRef& f) override { return inner_.ContentHash(f); }

 private:
  IFrameStoreAccess& inner_;
};

TEST_P(Extraction, ThePriorBoundsTheSearchAndTheAnswerStaysInsideIt) {
  // **The "bounds" half of "seeds and bounds, never truth", which was missing.** Without it, a
  // panorama with a half-turn symmetry makes ORB and AKAZE match features to their point-reflected
  // twins; the aliased rotation fits the pixels with ordinary inlier counts and sub-two-pixel
  // residuals, and was accepted. Measured on a twelve-frame ring before this bound existed: two of
  // eleven ORB steps and two of eleven AKAZE steps came back 175 to 179 degrees out about the
  // optical axis.
  //
  // Here the frames are identical, so the pixels say "identity" as loudly as pixels can. A prior a
  // half turn away must not drag the answer there — and equally must not be *followed*, which is
  // what the next test holds.
  FeatureRegistrationEngine engine = Engine();
  const Result<FeatureSet> a = engine.ExtractFeatures(Textured());
  const Result<FeatureSet> b = engine.ExtractFeatures(Textured());
  ASSERT_TRUE(a.ok() && b.ok());

  const Quat halfTurn = FromAxisAngle(Vec3{0, 0, 1}, std::numbers::pi);
  const Result<PairwiseResult> pair = engine.EstimatePairwise(a.value, b.value, halfTurn, Lens());

  // **Refusal, not an answer**, and asserting that is what makes this test able to fail. The first
  // version guarded its assertion with `if (pair.ok() && pair.value.accepted)` — which is never
  // true here, so the assertion never ran and no sabotage could reach it. Deleting the bound left
  // it green, which is how it was caught.
  //
  // The behaviour it now pins: the sensor says a half turn, the pixels say the identity, and those
  // cannot both be nearly right. The bound excludes the identity as a hypothesis, nothing else
  // gathers inliers near the prior — a half turn sends every bearing behind the camera, where
  // `Project` refuses — and the call refuses rather than picking a side. That is the honest
  // outcome: an engine that silently resolved this would be deciding whether to trust the sensor,
  // which is a policy and not an estimate.
  // **And it refuses for *this* reason.** `EXPECT_FALSE(pair.ok() && ...)` alone is satisfied by any
  // refusal at all — a lens the engine could not use, a frame it could not pin, a descriptor width
  // it did not recognise — none of which have anything to do with the bound this test is named
  // after. Naming the status and the sentence pins the refusal to the consensus search coming back
  // empty, which is the only outcome that means the bound did its work.
  ASSERT_FALSE(pair.ok()) << "the sensor and the pixels disagree by a half turn and this answered "
                             "anyway, with "
                          << pair.value.inliers << " inliers";
  // `RegistrationFailed` rather than `NotFound`: the code says "these two frames did not register",
  // which is what the bound produces here, and is distinct from the `NotFound` a frame store returns
  // for a handle naming nothing. The first version of this assertion had to substring-match
  // `status.detail` to tell those apart — a string the contract says is for a human and is never
  // parsed — which is a sign the code was carrying two meanings rather than that the test was
  // clumsy.
  EXPECT_EQ(pair.status.code, StatusCode::RegistrationFailed)
      << "refused, but not for the reason this test is about: " << pair.status.detail;

  ForgetOutputs(a.value);
  ForgetOutputs(b.value);
}

TEST_P(Extraction, APriorInsideTheBoundDoesNotOverrideThePixels) {
  // The other half, and the one the bound could have broken: "never as truth". A prior that is
  // wrong but *plausibly* wrong — well inside the bound — must lose to the pixels, or the bound
  // would have turned the sensor into the answer. Identical frames again, so the truth is the
  // identity and the prior is thirty degrees from it.
  FeatureRegistrationEngine engine = Engine();
  const Result<FeatureSet> a = engine.ExtractFeatures(Textured());
  const Result<FeatureSet> b = engine.ExtractFeatures(Textured());
  ASSERT_TRUE(a.ok() && b.ok());

  const Quat wrongButPlausible =
      FromAxisAngle(Vec3{0, 1, 0}, 30.0 * std::numbers::pi / 180.0);
  const Result<PairwiseResult> pair =
      engine.EstimatePairwise(a.value, b.value, wrongButPlausible, Lens());
  ASSERT_TRUE(pair.ok()) << pair.status.detail;

  const double fromIdentity =
      AngleBetween(pair.value.relativeRotation, Quat{1, 0, 0, 0}) * 180.0 / std::numbers::pi;
  EXPECT_LT(fromIdentity, 1.0)
      << "a thirty-degree prior moved the answer " << fromIdentity
      << " degrees off the identity the pixels show; the prior seeds and bounds, it is not truth";

  ForgetOutputs(a.value);
  ForgetOutputs(b.value);
}

TEST_P(Extraction, EstimatePairwiseForgetsNoneOfTheFourFramesItIsHanded) {
  // **Including on a refusal**, which is the half the contract had to spell out separately and the
  // half an implementation is most likely to get wrong: an error path that "cleans up" is the
  // natural thing to write and is exactly what must not happen here.
  //
  // Four refusals are driven, and the fourth is the one that matters. **Three of them refuse before
  // a single `Pin` is taken** — the empty set, the lens guard, the prior guard — so a `Forget` in
  // the pinned region could not have been reached by any of them, and an earlier version of this
  // comment claimed otherwise. The fourth hands the engine two feature sets made by *different*
  // detectors: all four frames are pinned and both keypoint sets are lifted to bearings before the
  // descriptor widths are compared, so the exit it takes is past every `BorrowedFrame` in the
  // function. That is the exit an over-helpful rollback would live in.
  //
  // Still not covered, and said here rather than implied: the exits inside the matching loop — too
  // few correspondences surviving the ratio test, and the consensus search coming back empty. Both
  // are post-pin and both are reached by other tests in this file, but not with a store that counts
  // `Forget`.
  CountingForgets counting{store};
  FeatureRegistrationEngine engine{counting, GetParam()};
  const Result<FeatureSet> a = engine.ExtractFeatures(Textured());
  const Result<FeatureSet> b = engine.ExtractFeatures(Textured());
  ASSERT_TRUE(a.ok()) << a.status.detail;
  ASSERT_TRUE(b.ok()) << b.status.detail;
  const int afterExtraction = counting.forgets;

  // A success.
  const Result<PairwiseResult> ok = engine.EstimatePairwise(a.value, b.value, Quat{1, 0, 0, 0},
                                                            Lens());
  EXPECT_TRUE(ok.ok()) << ok.status.detail;

  // An unusable lens, and an unusable prior. `Quat{0, 0, 0, 0}` spelled out, because `Quat{}` is
  // the *identity* — the struct default-initialises `w` to 1 — and the first version of this line
  // used it and was surprised to be refused nothing. A zero quaternion is not a rotation; the
  // identity is one, and an engine refusing it would be refusing the commonest prior there is.
  EXPECT_FALSE(engine.EstimatePairwise(a.value, b.value, Quat{1, 0, 0, 0}, Intrinsics{}).ok());
  EXPECT_FALSE(engine.EstimatePairwise(a.value, b.value, Quat{0, 0, 0, 0}, Lens()).ok());
  // A set with no rows, refused before anything is pinned.
  EXPECT_FALSE(engine.EstimatePairwise(FeatureSet{}, b.value, Quat{1, 0, 0, 0}, Lens()).ok());

  // **The post-pin refusal.** A second engine over the same counting store, with a detector that is
  // not this one: ORB is 32 bytes a row, AKAZE 61, and SIFT 128 floats, so whichever pair this
  // makes differs in width or in element type and the mismatch guard fires — after four `Pin`s and
  // two passes of `ReadBearings`.
  const FeatureDetector other =
      GetParam() == FeatureDetector::Sift ? FeatureDetector::Orb : FeatureDetector::Sift;
  FeatureRegistrationEngine foreign{counting, other};
  const Result<FeatureSet> c = foreign.ExtractFeatures(Textured());
  ASSERT_TRUE(c.ok()) << c.status.detail;
  const Result<PairwiseResult> mismatched =
      engine.EstimatePairwise(a.value, c.value, Quat{1, 0, 0, 0}, Lens());
  ASSERT_FALSE(mismatched.ok());
  EXPECT_NE(mismatched.status.detail.find("same detector"), std::string::npos)
      << "the mismatch was meant to refuse past the pins, and refused somewhere else: "
      << mismatched.status.detail;

  EXPECT_EQ(counting.forgets, afterExtraction)
      << "EstimatePairwise forgot " << (counting.forgets - afterExtraction)
      << " of the frames it was handed; they belong to the caller, refusal or not";

  // And they are still usable afterwards, which is the property the count is a proxy for: a caller
  // may estimate the same pair again.
  const Result<PairwiseResult> again = engine.EstimatePairwise(a.value, b.value, Quat{1, 0, 0, 0},
                                                               Lens());
  EXPECT_TRUE(again.ok()) << "the second estimate of the same pair failed, so the first consumed "
                             "its input: " << again.status.detail;

  ForgetOutputs(a.value);
  ForgetOutputs(b.value);
  ForgetOutputs(c.value);
}

/**
 * A refusal from the frame store arrives with the store's name on it, not this engine's.
 *
 * **`Status::component` says "which service reported it", and rebuilding a status loses that.**
 * `EstimatePairwise` used to return `Err<PairwiseResult>(pinned->status.code, kComponent, ...)`,
 * which kept the code and the detail and overwrote the one field that answers *who*. So a caller
 * chasing a failed pin was told `FeatureRegistrationEngine` when the store had said `NotFound: no
 * such frame`. `Extract`, in the same class, already returned the status whole — two methods
 * disagreeing about the same promise, with no test on either to notice.
 *
 * Driven with a handle naming no frame, which is exactly the "could not be pinned" case a caller
 * reaches by holding a `FeatureSet` past a `Forget`.
 */
TEST_P(Extraction, APinRefusalKeepsTheStoresOwnComponent) {
  FeatureRegistrationEngine engine = Engine();
  const Result<FeatureSet> a = engine.ExtractFeatures(Textured());
  const Result<FeatureSet> b = engine.ExtractFeatures(Textured());
  ASSERT_TRUE(a.ok() && b.ok());

  FeatureSet dangling = a.value;
  dangling.keypoints = FrameRef{};

  const Result<PairwiseResult> pair =
      engine.EstimatePairwise(dangling, b.value, Quat{1, 0, 0, 0}, Lens());
  ASSERT_FALSE(pair.ok());
  EXPECT_EQ(pair.status.code, StatusCode::NotFound) << pair.status.detail;
  EXPECT_NE(pair.status.component, "FeatureRegistrationEngine")
      << "the engine put its own name on a refusal the store issued";

  ForgetOutputs(a.value);
  ForgetOutputs(b.value);
}

/**
 * Every bounds guard on the way into `EstimatePairwise`, driven.
 *
 * **Seven, and none was tested.** The docblock said five for two rounds and the footer below now
 * enumerates seven; all seven existed when this test was written, so five was a miscount rather
 * than a change. A reviewer deleted the lot — both keypoint guards
 * and all three descriptor ones — and 106 tests stayed green, the accuracy measurement included;
 * the same input then gave ASan `heap-buffer-overflow READ of size 8, 0 bytes after a 3768-byte
 * region` inside `ReadBearings`. Correct code with nothing defending it is one careless edit away
 * from being incorrect code, and on this branch that edit has happened twice.
 *
 * A `FeatureSet` is a value its caller fills in, so every case here is reachable without a
 * conspiring store: a caller that mislays a stride, doubles a count, or hands on a set built for a
 * different detector. What is asserted is a refusal rather than a message — a guard's job is to not
 * read past the frame — and the detail is checked only where two guards would otherwise be
 * indistinguishable.
 */
TEST_P(Extraction, EveryBoundsGuardRefusesRatherThanReadingPastTheFrame) {
  FeatureRegistrationEngine engine = Engine();
  const Result<FeatureSet> a = engine.ExtractFeatures(Textured());
  const Result<FeatureSet> b = engine.ExtractFeatures(Textured());
  ASSERT_TRUE(a.ok() && b.ok());
  ASSERT_GT(a.value.count, 3);

  const auto refuses = [&](FeatureSet doctored, const char* what) {
    const Result<PairwiseResult> pair =
        engine.EstimatePairwise(doctored, b.value, Quat{1, 0, 0, 0}, Lens());
    EXPECT_FALSE(pair.ok()) << what << ": answered instead of refusing, with "
                            << pair.value.inliers << " inliers";
    return pair.status.detail;
  };

  // **A keypoint stride narrower than one row.** Eight bytes is a row — `FeatureSet::keypoints`
  // spells that out in the contract — so seven describes a frame whose rows overlap, which no store
  // produces and a caller can certainly claim.
  {
    FeatureSet narrow = a.value;
    narrow.keypoints.stride = 7;
    EXPECT_NE(refuses(narrow, "a keypoint stride under one row").find("stride"), std::string::npos);
  }
  // **More keypoint rows than the frame holds.** This is the one whose absence ASan caught: without
  // it the read walks `count * stride` bytes into a frame that has fewer, and the report was
  // `heap-buffer-overflow READ of size 8, 0 bytes after a 3768-byte region`.
  {
    FeatureSet tooMany = a.value;
    tooMany.count = a.value.count * 4;
    // **And it has to be the *keypoint* frame's guard that refuses.** Without this line the case was
    // green with its own guard deleted: `ReadDescriptors` refuses a few lines later, about the
    // descriptor frame, *after* `ReadBearings` has already walked `4 * count * 8` bytes of a frame
    // holding `count * 8`. The refusal arrives, the read has already happened, and the only thing
    // that can tell the two apart is which frame the message names. Two sibling cases in this test
    // got this assertion when they were written and this one did not, which is the enumeration
    // failure this repository keeps having: the copy in front of me was fixed and its neighbour
    // was not.
    EXPECT_NE(refuses(tooMany, "four times as many keypoint rows as the frame holds")
                  .find("keypoint frame holds fewer bytes"),
              std::string::npos);
  }
  // **A descriptor pitch wider than the frame's real rows, claimed on *both* sets.** Doctoring one
  // side only never reaches these guards: the width comparison refuses the pair first, because a
  // 64-byte row on one side and a 32-byte row on the other is exactly what "not made by the same
  // detector" means. Both sides claim it, the widths agree, and the row-count division is then the
  // only thing between the matcher and a read past the end of the allocation.
  {
    FeatureSet wideA = a.value;
    FeatureSet wideB = b.value;
    wideA.descriptors.stride = a.value.descriptors.stride * 2;
    wideB.descriptors.stride = b.value.descriptors.stride * 2;
    const Result<PairwiseResult> pair =
        engine.EstimatePairwise(wideA, wideB, Quat{1, 0, 0, 0}, Lens());
    ASSERT_FALSE(pair.ok()) << "a doubled descriptor pitch on both sides answered instead of "
                               "refusing, with " << pair.value.inliers << " inliers";
    // **And the refusal has to be *this* one.** Removing the guard does not make the call answer:
    // it makes it read past the frame and refuse anyway, because descriptors assembled from
    // whatever follows the allocation match nothing. `EXPECT_FALSE(ok())` is therefore satisfied
    // with the guard and without it, and proves nothing either way — the guard's own sentence is
    // what separates a bounds check from a coincidence. Found by running the sabotage: with all
    // four guards removed this case still "passed".
    EXPECT_NE(pair.status.detail.find("its own row count"), std::string::npos)
        << "refused, but not by the bounds check this case exists for: " << pair.status.detail;
  }
  // **A descriptor pitch one byte over, on both sides.** An earlier comment here claimed SIFT would
  // reach the divisibility guard — 513 bytes where an element is four — and it does not: the
  // row-count division is checked *first*, and a 513-byte pitch over a frame of 512-byte rows fails
  // it, on all three detectors. So this case drives one guard, not two. The assertion below still
  // accepts either sentence, because naming the wrong one is exactly how the previous comment went
  // stale.
  {
    FeatureSet raggedA = a.value;
    FeatureSet raggedB = b.value;
    raggedA.descriptors.stride = a.value.descriptors.stride + 1;
    raggedB.descriptors.stride = b.value.descriptors.stride + 1;
    const Result<PairwiseResult> pair =
        engine.EstimatePairwise(raggedA, raggedB, Quat{1, 0, 0, 0}, Lens());
    ASSERT_FALSE(pair.ok())
        << "a descriptor pitch one byte over answered instead of refusing, with "
        << pair.value.inliers << " inliers";
    const bool byAGuard = pair.status.detail.find("whole number of elements") != std::string::npos ||
                          pair.status.detail.find("its own row count") != std::string::npos;
    EXPECT_TRUE(byAGuard) << "refused, but not by either bounds check this case can reach: "
                          << pair.status.detail;
  }

  // **A descriptor pitch that is not a whole number of elements *and still fits the frame*.** The
  // case above cannot reach that guard: inflating the pitch trips the row-count division first, on
  // every detector. Shrinking it does reach it — a SIFT set claiming 510-byte rows where the frame
  // holds 512, with `count` halved so the smaller pitch still covers the bytes. 510 is not a whole
  // number of four-byte floats, and the row-count division is satisfied, so the divisibility guard
  // is the one that answers.
  //
  // Only SIFT can reach it: for the byte detectors every pitch divides by one. So the assertion is
  // scoped to the detector it applies to rather than weakened to something all three satisfy.
  if (GetParam() == FeatureDetector::Sift) {
    FeatureSet ragged = a.value;
    FeatureSet raggedB = b.value;
    ragged.descriptors.stride = a.value.descriptors.stride - 2;
    raggedB.descriptors.stride = b.value.descriptors.stride - 2;
    ragged.count = a.value.count / 2;
    raggedB.count = b.value.count / 2;
    const Result<PairwiseResult> pair =
        engine.EstimatePairwise(ragged, raggedB, Quat{1, 0, 0, 0}, Lens());
    ASSERT_FALSE(pair.ok()) << "a 510-byte pitch over four-byte elements answered instead of "
                               "refusing";
    EXPECT_NE(pair.status.detail.find("whole number of elements"), std::string::npos)
        << "refused, but not by the divisibility check this case exists for: " << pair.status.detail;
  }

  // **Seven guards on this path, four driven above, three shadowed — and I counted them wrong
  // twice before a reviewer counted them properly.** Written out, because "all the bounds guards"
  // is the kind of claim that decays into an unexamined comfort:
  //
  // Driven: the keypoint stride floor, the keypoint row count, the descriptor row count, and
  // element divisibility. Removing any one of those four on its own fails a case above.
  //
  // Not driven, each because something earlier refuses first — not because the guard is redundant:
  //   - `count <= 0 || rows.empty()` in `ReadDescriptors`: `EstimatePairwise` refuses a set with no
  //     rows before either reader runs.
  //   - a descriptor pitch of zero: needs `count` to exceed the pinned bytes, and `ReadBearings`
  //     runs first on the same `count`.
  //   - a column count past `INT_MAX`: `FrameRef::stride` is an `int32_t`, so a declared pitch
  //     cannot reach it; the only route is the unset-stride fallback on a descriptor frame over two
  //     gibibytes, which a test has no business allocating.
  //
  // All four are kept. The call order is not a promise, and a future reader of these frames may not
  // have a keypoint guard in front of it.

  ForgetOutputs(a.value);
  ForgetOutputs(b.value);
}

/**
 * A descriptor row is as wide as the frame says, not as wide as the pinned bytes divide out to.
 *
 * **The same defect `ReadBearings` was fixed for, in the reader beside it.** The keypoint reader
 * now takes its pitch from `FrameRef::stride`; the descriptor reader still computed
 * `pinned.size() / count`, which is the row width only when the pin returns exactly the rows the
 * set claims. Hand it a set claiming fewer rows than the frame holds — which a caller can do, since
 * `FeatureSet` is a value it owns and fills in — and every row came out twice as wide, built from
 * the bytes of two.
 *
 * Driven by halving `count` rather than by a padding store, because this is a statement about the
 * *set* disagreeing with its frame and not about how the store allocates. The two sets are
 * extracted from identical frames and one is doctored, so under the old reader `a` is 64 bytes a
 * row against `b`'s 32 and the mismatch guard refuses the pair; under the new one both are 32 and
 * the pair registers to the identity it should.
 */

TEST_P(Extraction, TheDescriptorWidthComesFromTheFrameAndNotFromTheByteCount) {
  FeatureRegistrationEngine engine = Engine();
  const Result<FeatureSet> a = engine.ExtractFeatures(Textured());
  const Result<FeatureSet> b = engine.ExtractFeatures(Textured());
  ASSERT_TRUE(a.ok() && b.ok());
  ASSERT_GT(a.value.count, 3) << "too few features to halve";

  FeatureSet halved = a.value;
  halved.count = a.value.count / 2;

  const Result<PairwiseResult> pair =
      engine.EstimatePairwise(halved, b.value, Quat{1, 0, 0, 0}, Lens());
  ASSERT_TRUE(pair.ok()) << "the rows were read at the wrong width: " << pair.status.detail;
  EXPECT_TRUE(pair.value.accepted);
  EXPECT_LT(AngleBetween(pair.value.relativeRotation, Quat{1, 0, 0, 0}) * 180.0 /
                std::numbers::pi,
            0.5);

  ForgetOutputs(a.value);
  ForgetOutputs(b.value);
}

TEST_P(Extraction, RegisteringAFrameAgainstItselfIsTheIdentity) {
  // **The invariant the engineering skill names for exactly this case**, and the only assertion
  // about a rotation that can be written before any dataset exists to measure against: a frame
  // registered against itself has turned by nothing, and every match is an inlier. The descriptors
  // are identical, so the ratio test has a perfect answer for every row and RANSAC has no outlier
  // to reject — if this one is not exact, nothing further along will be.
  //
  // Two frames rather than one `FeatureSet` passed twice, because the contract allows a caller to
  // estimate the same set against itself and an implementation that aliased its two inputs would
  // pass this while failing every real pair.
  FeatureRegistrationEngine engine = Engine();
  const Result<FeatureSet> a = engine.ExtractFeatures(Textured());
  const Result<FeatureSet> b = engine.ExtractFeatures(Textured());
  ASSERT_TRUE(a.ok()) << a.status.detail;
  ASSERT_TRUE(b.ok()) << b.status.detail;
  ASSERT_GT(a.value.count, 0);
  ASSERT_EQ(a.value.count, b.value.count);

  // **A prior that is not the answer**, which is what makes this an assertion about the fit. The
  // first version passed the identity — and `FitRotation` seeds its search with the prior, so the
  // correct answer was sitting in `best` before a pixel was read. A reviewer gutted the estimator to
  // `return the prior` and this test passed on all three detectors, `medianResidualPx` included.
  // Ten degrees is comfortably inside the 45-degree bound, so the identity still has to be *found*
  // rather than handed over, and the pixels are unanimous about it.
  const Quat offset = FromAxisAngle(Vec3{0.577, 0.577, 0.577}, 10.0 * std::numbers::pi / 180.0);
  const Result<PairwiseResult> pair = engine.EstimatePairwise(a.value, b.value, offset, Lens());
  ASSERT_TRUE(pair.ok()) << pair.status.detail;

  // The rotation, as an angle rather than component by component: the double cover makes -q the
  // same rotation as q, so comparing `w` to 1 would fail on a correct answer half the time.
  const Quat& turn = pair.value.relativeRotation;
  const double norm = std::sqrt(turn.w * turn.w + turn.x * turn.x + turn.y * turn.y +
                                turn.z * turn.z);
  ASSERT_GT(norm, 0.0) << "a zero quaternion is not a rotation";
  const double angleDeg =
      2.0 * std::acos(std::min(1.0, std::abs(turn.w) / norm)) * 180.0 / std::numbers::pi;
  EXPECT_LT(angleDeg, 1e-6) << "a frame against itself has turned by nothing, not " << angleDeg
                            << " degrees";

  EXPECT_TRUE(pair.value.accepted);
  EXPECT_EQ(pair.value.a.value, a.value.frame.value);
  EXPECT_EQ(pair.value.b.value, b.value.frame.value);
  EXPECT_GT(pair.value.inliers, 0);
  EXPECT_LT(pair.value.medianResidualPx, 1e-6)
      << "identical descriptors at identical keypoints leave no residual";

  ForgetOutputs(a.value);
  ForgetOutputs(b.value);
}

TEST_P(Extraction, ARollbackHoldingPinsGivesTheBytesBackBeforeItForgetsThem) {
  // `OwnedFrame`'s destructor releases and only then forgets, and `Forget` on a pinned frame fails.
  // Swapping the two lines therefore orphans every frame a *pinned* rollback unwinds — measured
  // here at 13,776 / 9,095 / 53,064 bytes per refusal across ORB, AKAZE and SIFT — while leaving
  // every test in the repository green, because nothing else reaches a rollback with a pin held.
  //
  // An earlier draft of this comment carried 7,872 / 6,527 / 50,688, which are the figures from the
  // *other* rollback test: they count one unpadded descriptor frame, and this test unwinds two
  // frames whose rows the store has padded. Numbers measured on one arrangement and quoted under
  // another is the mistake this repository keeps making, and it was made here by the commit that
  // added the comment warning about it.
  //
  // A padding store is what gets there: the packing guard runs after both `Pin`s, so this is the
  // one refusal in the engine that unwinds with the pins live.
  const FrameRef source = Textured();
  const Result<std::span<uint8_t>> bytes = store.Pin(source);
  ASSERT_TRUE(bytes.ok()) << bytes.status.detail;

  PaddingFrameStore padding{1 << 24};
  const Result<FrameRef> copied = padding.Allocate(kWidth, kHeight, PixelFormat::RGBA8);
  ASSERT_TRUE(copied.ok()) << copied.status.detail;
  {
    const Result<std::span<uint8_t>> into = padding.Pin(copied.value);
    ASSERT_TRUE(into.ok()) << into.status.detail;
    ASSERT_GE(into.value.size(), bytes.value.size());
    std::memcpy(into.value.data(), bytes.value.data(), bytes.value.size());
    EXPECT_TRUE(padding.Release(copied.value).ok());
  }
  EXPECT_TRUE(store.Release(source).ok());

  const Result<FrameStoreBudget> before = padding.Budget();
  ASSERT_TRUE(before.ok());
  FeatureRegistrationEngine engine{padding, GetParam()};
  const Result<FeatureSet> refused = engine.ExtractFeatures(copied.value);
  ASSERT_FALSE(refused.ok()) << "a padded store should have been refused by the packing guard";
  EXPECT_EQ(refused.status.code, StatusCode::Internal) << refused.status.detail;

  const Result<FrameStoreBudget> after = padding.Budget();
  ASSERT_TRUE(after.ok());
  EXPECT_EQ(after.value.heapUsedBytes, before.value.heapUsedBytes)
      << "the rollback forgot a frame while it was still pinned, so the store kept the bytes";
}

TEST_P(Extraction, LeavesThePixelsItReadExactlyAsItFoundThem) {
  // `FeatureSet` promises the frame's pixels are unchanged, and nothing asked.
  //
  // **Both formats, because only one of them aliases.** An RGBA8 frame goes through `cvtColor` into
  // a fresh Mat, so a detector writing in place would scribble on the copy; the `Gray8` branch hands
  // `cv::Mat` the pinned span itself, which is the case the promise is actually about and the one an
  // earlier version of this test never reached — a scribble there left every test green.
  for (const PixelFormat format : {PixelFormat::RGBA8, PixelFormat::Gray8}) {
    const Result<FrameRef> allocated = store.Allocate(kWidth, kHeight, format);
    ASSERT_TRUE(allocated.ok()) << allocated.status.detail;
    {
      const Result<std::span<uint8_t>> bytes = store.Pin(allocated.value);
      ASSERT_TRUE(bytes.ok()) << bytes.status.detail;
      for (int32_t y = 0; y < kHeight; ++y) {
        for (int32_t x = 0; x < kWidth; ++x) {
          const uint8_t v = TexturedLuma(x, y);
          uint8_t* px = bytes.value.data() + static_cast<size_t>(y) * allocated.value.stride
                      + static_cast<size_t>(x) * (format == PixelFormat::RGBA8 ? 4 : 1);
          px[0] = v;
          if (format == PixelFormat::RGBA8) { px[1] = v; px[2] = v; px[3] = 255; }
        }
      }
      EXPECT_TRUE(store.Release(allocated.value).ok());
    }

    const Result<uint64_t> before = store.ContentHash(allocated.value);
    ASSERT_TRUE(before.ok()) << before.status.detail;
    const Result<FeatureSet> features = Engine().ExtractFeatures(allocated.value);
    ASSERT_TRUE(features.ok()) << features.status.detail;
    const Result<uint64_t> after = store.ContentHash(allocated.value);
    ASSERT_TRUE(after.ok()) << after.status.detail;

    EXPECT_EQ(before.value, after.value)
        << "extraction wrote into the frame it was reading ("
        << (format == PixelFormat::RGBA8 ? "RGBA8" : "Gray8") << ")";
    ForgetOutputs(features.value);
    EXPECT_TRUE(store.Forget(allocated.value).ok());
  }
}

TEST_P(Extraction, ReadingASpilledFrameLeavesItInTheHeap) {
  // Extraction pins, and pinning faults a spilled frame back in and leaves it resident. That is
  // what `FeatureSet`'s header means by "unchanged but not untouched", and it has a sharp edge
  // worth pinning down rather than discovering: a refusal for want of heap makes the heap *fuller*,
  // so extracting across a sphere of cold frames ratchets the ceiling shut one frame at a time.
  //
  // Asserted rather than fixed. Demoting the frame again on the way out would be the engine undoing
  // something it did not set up — the rollback-that-destroys shape this codebase has been bitten by
  // — and the caller that cooled the frame is the one that knows whether it still wants it cold.
  FakeSpillSink sink;
  MemoryFrameStoreAccess spilling{1 << 24, &sink};
  const Result<FrameRef> frame = spilling.Allocate(kWidth, kHeight, PixelFormat::RGBA8);
  ASSERT_TRUE(frame.ok()) << frame.status.detail;
  {
    const Result<std::span<uint8_t>> bytes = spilling.Pin(frame.value);
    ASSERT_TRUE(bytes.ok()) << bytes.status.detail;
    for (size_t at = 0; at < bytes.value.size(); ++at) {
      bytes.value[at] = static_cast<uint8_t>(TexturedLuma(static_cast<int32_t>(at / 4) % kWidth,
                                                          static_cast<int32_t>(at / 4) / kWidth));
    }
    EXPECT_TRUE(spilling.Release(frame.value).ok());
  }
  ASSERT_TRUE(spilling.Demote(frame.value, Residency::Spilled).ok());
  const Result<Residency> cold = spilling.ResidencyOf(frame.value);
  ASSERT_TRUE(cold.ok());
  ASSERT_EQ(cold.value, Residency::Spilled) << "the fixture did not manage to cool the frame";

  FeatureRegistrationEngine engine{spilling, GetParam()};
  const Result<FeatureSet> features = engine.ExtractFeatures(frame.value);
  ASSERT_TRUE(features.ok()) << features.status.detail;

  const Result<Residency> after = spilling.ResidencyOf(frame.value);
  ASSERT_TRUE(after.ok());
  // `HeapEncoded` exactly, not merely "not Spilled": a leaked pin leaves it `HeapPinned`, which is
  // also not Spilled, so the weaker form passed on a defect this suite has a separate test for.
  EXPECT_EQ(after.value, Residency::HeapEncoded)
      << "if this ever comes back Spilled, the contract comment on FeatureSet is the thing to fix";
  // From this store, not the fixture's — `ForgetOutputs` would ask the wrong one and be told the
  // frames do not exist, which is a failure about the test rather than about the engine.
  if (features.value.count > 0) {
    EXPECT_TRUE(spilling.Forget(features.value.descriptors).ok());
    EXPECT_TRUE(spilling.Forget(features.value.keypoints).ok());
  }
}

TEST_P(Extraction, RefusesAStoreThatHandsBackFewerBytesThanItWasAskedFor) {
  // The other half of the store check, which `PaddingFrameStore`'s row padding could not reach: a
  // padded frame is always *larger* than asked, so every size comparison passed and only the stride
  // comparison did any work. Deleting the size half left all 54 registration tests green.
  //
  // A store one row short keeps `stride == width` and fails only on the total, which is the exact
  // shape the `memcpy` loop below it would otherwise walk off the end of.
  const FrameRef source = Textured();
  const Result<std::span<uint8_t>> bytes = store.Pin(source);
  ASSERT_TRUE(bytes.ok()) << bytes.status.detail;

  PaddingFrameStore shorting{1 << 24, PaddingFrameStore::Lie::ShortRows};
  const Result<FrameRef> copied = shorting.Allocate(kWidth, kHeight, PixelFormat::RGBA8);
  ASSERT_TRUE(copied.ok()) << copied.status.detail;
  {
    const Result<std::span<uint8_t>> into = shorting.Pin(copied.value);
    ASSERT_TRUE(into.ok()) << into.status.detail;
    std::memcpy(into.value.data(), bytes.value.data(),
                std::min(into.value.size(), bytes.value.size()));
    EXPECT_TRUE(shorting.Release(copied.value).ok());
  }
  EXPECT_TRUE(store.Release(source).ok());

  const Result<FeatureSet> refused =
      FeatureRegistrationEngine{shorting, GetParam()}.ExtractFeatures(copied.value);
  ASSERT_FALSE(refused.ok()) << "a short frame was copied into as though it were the right size";
  EXPECT_EQ(refused.status.code, StatusCode::Internal) << refused.status.detail;
  // How this one fails when it fails, recorded because the signature is easy to misread: without
  // the guard the copy loop runs off the short frame and glibc aborts with `corrupted size vs.
  // prev_size` — **exit 134, no `[  FAILED  ]` line, and the rest of the suite never runs.** A
  // sabotage of this check looks like a clean run to anything reading gtest's summary.
}

INSTANTIATE_TEST_SUITE_P(EveryDetector, Extraction,
                         // From the engine's own list rather than restated here, and that list is
                         // held to the enum's size by a `static_assert`, so a detector added to
                         // `FeatureDetector` cannot reach `Make()` while reaching none of these
                         // tests. An earlier version of this comment claimed that was already true
                         // of `ValuesIn` alone; a reviewer added a fourth enumerator and showed it
                         // was not.
                         ::testing::ValuesIn(kAllFeatureDetectors),
                         [](const ::testing::TestParamInfo<FeatureDetector>& info) {
                           switch (info.param) {
                             case FeatureDetector::Orb: return "Orb";
                             case FeatureDetector::Akaze: return "Akaze";
                             case FeatureDetector::Sift: return "Sift";
                             case FeatureDetector::Count: break;
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

/**
 * The degeneracy test says the same thing about the same bearings however many there are.
 *
 * **The point is the invariance, so the test is written as one.** The covariance the Kabsch fit
 * builds is a sum over correspondences, so doubling the number of bearings roughly doubles every
 * singular value while the *shape* they describe is unchanged. An absolute threshold — which is
 * what this was — therefore answers a different question at a three-point minimal sample than at a
 * sixty-inlier refit, and both are shapes this engine fits on the same call.
 */
TEST(Degeneracy, ScalingEveryBearingSetTheSameWayDoesNotChangeTheAnswer) {
  // A plausible minimal sample: three unit bearings a few degrees apart, so the second axis is
  // resolved at a few percent of the first.
  const double largest = 2.9;
  const double second = 0.04;
  EXPECT_TRUE(BearingsSpanAPlane(largest, second));
  // The same shape, sixty inliers instead of three. Under the old absolute bound this was the case
  // that drifted: everything got twenty times larger and the fixed 1e-9 floor twenty times easier
  // to clear.
  EXPECT_TRUE(BearingsSpanAPlane(largest * 20.0, second * 20.0));

  // Bearings on one line: the second axis is a rounding error rather than a direction, and the
  // rotation about that line is unconstrained.
  EXPECT_FALSE(BearingsSpanAPlane(3.0, 3.0 * 1e-12));
  // **The same degenerate shape at twenty times the count, which the absolute bound accepted.**
  // 60 * 1e-12 is 6e-11 — under 1e-9, so the old predicate refused this one too. Scale it the other
  // way and the old one breaks: see the next assertion.
  EXPECT_FALSE(BearingsSpanAPlane(60.0, 60.0 * 1e-12));
  // Sixty bearings on one line, with the second axis at 1e-10 of the first — which is 6e-9,
  // *above* the old absolute floor, so the old predicate called this a plane and handed the fit a
  // rotation with a free axis. This assertion is the whole of the finding.
  EXPECT_FALSE(BearingsSpanAPlane(60.0, 6e-9));

  // Nothing to fit: no bearings, or every one of them the zero vector. There is no zero guard any
  // more — a reviewer showed the one that was here could be deleted with every test still green,
  // because `cv::SVD` orders the singular values so `second <= largest`, and `0 > 1e-9 * 0` is
  // false on its own. This assertion stays as the statement that the comparison is total at the
  // bottom of its range, which is a different claim from "a guard runs".
  EXPECT_FALSE(BearingsSpanAPlane(0.0, 0.0));
}

/**
 * The search budget, checked against the textbook it claims to come from.
 *
 * **This exists because the accuracy test cannot see the difference.** The budget was a written-down
 * 200 until a perturbed prior exposed what it cost, and raising it turned one ORB step from a
 * two-correspondence consensus into a thirteen-correspondence one — but ORB still registers eight
 * of eleven steps either way, so every assertion in the accuracy test reads the same before and
 * after. A wrong budget is invisible there, which is the argument for pinning the arithmetic where
 * it is visible.
 *
 * The expected values are worked from `ceil(log(1 - 0.99) / log(1 - w^3))` by hand rather than
 * printed from the function, since a test that asks the code what it says is a test of nothing.
 */
TEST(SampleBudget, TheDrawsAreTheOnesNinetyNinePercentConfidenceNeeds) {
  // w = 0.5: one triple in eight is all-inlier, and 35 draws miss them all with probability 0.01.
  // This is the textbook figure the first version wrote down as a constant for every ratio.
  EXPECT_EQ(RansacSampleBudget(0.5), 35);
  // w = 0.2 — the acceptance gate. Sixteen times as many draws as w = 0.5, which is the size of the
  // mistake the constant was making on this dataset.
  EXPECT_EQ(RansacSampleBudget(0.2), 574);
  // w = 0.3, between the two, so the interpolation is pinned and not just the ends.
  EXPECT_EQ(RansacSampleBudget(0.3), 169);
  // Nearly every correspondence agrees: four draws are enough, and the loop leaves almost at once.
  EXPECT_EQ(RansacSampleBudget(0.9), 4);
}

/**
 * A ratio under the gate asks for the gate's budget, and a perfect one asks for nothing.
 *
 * Separate from the arithmetic above because these are the two ends the loop actually hands it:
 * `RansacSampleBudget(0.0)` opens the search, and the observed ratio it passes in on every
 * improvement can be anything from two-in-a-hundred-and-fifty to all of them.
 */
TEST(SampleBudget, BelowTheGateItAsksForTheGateAndAboveEverythingItAsksForNothing) {
  // Searching past the draws that would find a 20% consensus buys nothing: a smaller one would be
  // refused even if found. So these are not "more thorough", they are the same number.
  EXPECT_EQ(RansacSampleBudget(0.0), RansacSampleBudget(0.2));
  EXPECT_EQ(RansacSampleBudget(2.0 / 150.0), RansacSampleBudget(0.2));
  // Total, not clamped-and-hoped: w = 1 puts `1 - w^3` at zero, where the logarithm is not finite.
  EXPECT_EQ(RansacSampleBudget(1.0), 0);
}

TEST(DetectorCoverage, AValueThatIsNotADetectorIsRefusedRatherThanBuilt) {
  // `FeatureDetector::Count` exists so `kAllFeatureDetectors` can be checked against the enum's
  // size rather than remembered. That buys a value which is not a detector, and this repository is
  // strict about sentinels for good reason — so the path it takes is asserted rather than assumed.
  //
  // `Make()` answers null for it and `ExtractFeatures` turns that into `Unsupported`, which is the
  // refusal that already existed for a detector the engine could not build. Before this, that
  // branch was unreachable: every enumerator named a real detector.
  MemoryFrameStoreAccess store{1 << 20};
  const Result<FrameRef> allocated = store.Allocate(64, 64, PixelFormat::RGBA8);
  ASSERT_TRUE(allocated.ok()) << allocated.status.detail;

  FeatureRegistrationEngine engine{store, FeatureDetector::Count};
  const Result<FeatureSet> features = engine.ExtractFeatures(allocated.value);
  EXPECT_FALSE(features.ok()) << "a value that names no detector must not produce features";
  EXPECT_EQ(features.status.code, StatusCode::Unsupported);

  // And it gives the frame back on the way out, like every other refusal in this file.
  const Result<Residency> residency = store.ResidencyOf(allocated.value);
  ASSERT_TRUE(residency.ok());
  EXPECT_EQ(residency.value, Residency::HeapEncoded) << "the refused extraction left it pinned";
  EXPECT_TRUE(store.Forget(allocated.value).ok());
}

TEST(DetectorDefaults, TheAkazeParametersCopiedFromOpenCvAreStillOpenCvs) {
  // `Make()` restates seven of OpenCV's AKAZE defaults because the cap is the eighth argument, and
  // the oracle in this file restates the same seven a second time. Those two copies are compared
  // with each other on every run — and never with the authority they were copied from.
  //
  // The direction that leaves open is the one that matters: if OpenCV's defaults move under an SHA
  // bump, both copies keep the old values, agree perfectly, and every test here passes while AKAZE
  // runs with parameters nobody chose. "Which detector wins is a measurement" would then be
  // measuring a non-default AKAZE against a default ORB and a default SIFT.
  //
  // So this asks OpenCV. It does not loosen ADR 0005's pin — it makes a bump of that pin arrive as
  // a red test naming the parameter, instead of as a silent change to what "AKAZE" means here.
  // `max_points` is deliberately absent: that is the one value this project means to override.
  const cv::Ptr<cv::AKAZE> theirs = cv::AKAZE::create();
  ASSERT_TRUE(theirs);
  EXPECT_EQ(theirs->getDescriptorType(), cv::AKAZE::DESCRIPTOR_MLDB);
  EXPECT_EQ(theirs->getDescriptorSize(), 0);
  EXPECT_EQ(theirs->getDescriptorChannels(), 3);
  EXPECT_FLOAT_EQ(theirs->getThreshold(), 0.001F);
  EXPECT_EQ(theirs->getNOctaves(), 4);
  EXPECT_EQ(theirs->getNOctaveLayers(), 4);
  EXPECT_EQ(theirs->getDiffusivity(), cv::KAZE::DIFF_PM_G2);
}

TEST(NullRegistration, RefusesEverythingRatherThanPretending) {
  // Kept beside the real one so the pair is visible: the null engine is what a WASM build gets
  // (ADR 0052), and it refuses rather than returning an identity that would look like a stitch.
  //
  // **"Everything" used to mean one of the three methods.** A reviewer made `EstimatePairwise` and
  // `Refine` return `Ok` — the identity registration this class's own header calls worse than a
  // refusal — and all 707 tests passed. The two *are* covered by
  // `RefinementRefusesRatherThanAnswering`, but that is a `TEST_P` over
  // `FeatureRegistrationEngine`, which exists only where OpenCV does and which no composition root
  // selects; `bridge/runtime.h` holds this one. So the engine every browser actually gets had its
  // two most dangerous methods asserted nowhere.
  NullRegistrationEngine engine;
  // The code, not just the refusal. A reviewer changed this one to `InvalidArgument` and all 714
  // tests stayed green: the two methods added when this gap was first closed assert their codes and
  // the one that was already here did not, so the fix covered its own additions and not the line it
  // was standing next to.
  const Result<FeatureSet> extracted = engine.ExtractFeatures(FrameRef{});
  EXPECT_FALSE(extracted.ok());
  EXPECT_EQ(extracted.status.code, StatusCode::Unsupported);

  const Result<PairwiseResult> pair = engine.EstimatePairwise(FeatureSet{}, FeatureSet{}, Quat{}, Intrinsics{});
  EXPECT_FALSE(pair.ok()) << "an identity rotation here would look like a registration";
  EXPECT_EQ(pair.status.code, StatusCode::Unsupported);

  const Result<GlobalSolution> refined = engine.Refine({}, {}, Intrinsics{});
  EXPECT_FALSE(refined.ok()) << "an empty solution here would look like a solved sphere";
  EXPECT_EQ(refined.status.code, StatusCode::Unsupported);
}

}  // namespace
}  // namespace sphanorama

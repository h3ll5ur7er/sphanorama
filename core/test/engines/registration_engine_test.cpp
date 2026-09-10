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

  /** A frame whose RGBA8 luma at (x, y) is whatever `paint` says. */
  template <typename Paint>
  FrameRef Painted(Paint paint) {
    const Result<FrameRef> allocated = store.Allocate(kWidth, kHeight, PixelFormat::RGBA8);
    EXPECT_TRUE(allocated.ok()) << allocated.status.detail;
    const FrameRef frame = allocated.value;
    const Result<std::span<uint8_t>> pinned = store.Pin(frame);
    EXPECT_TRUE(pinned.ok()) << pinned.status.detail;
    for (int32_t y = 0; y < kHeight; ++y) {
      for (int32_t x = 0; x < kWidth; ++x) {
        const uint8_t v = paint(x, y);
        uint8_t* px = pinned.value.data() + static_cast<size_t>(y) * frame.stride + x * 4;
        px[0] = px[1] = px[2] = v;
        px[3] = 255;
      }
    }
    store.Release(frame);
    return frame;
  }

  FrameRef Textured() { return Painted(TexturedLuma); }

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
    return Painted([](int32_t, int32_t) -> uint8_t { return 128; });
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

TEST(TexturedFrame, NoTwoTilesAreIdentical) {
  // The property `Textured()` exists to have, asserted rather than described. Matching arrives in
  // the next increment and a repeating texture would let a wrong correspondence score as a right
  // one — so this is the test that has to be here before that code is, not after it.
  std::vector<std::vector<uint8_t>> tiles;
  for (int32_t ty = 0; ty < kHeight / kTile; ++ty) {
    for (int32_t tx = 0; tx < kWidth / kTile; ++tx) {
      std::vector<uint8_t> tile;
      tile.reserve(static_cast<size_t>(kTile) * kTile);
      for (int32_t j = 0; j < kTile; ++j) {
        for (int32_t i = 0; i < kTile; ++i) tile.push_back(TexturedLuma(tx * kTile + i, ty * kTile + j));
      }
      for (size_t earlier = 0; earlier < tiles.size(); ++earlier) {
        EXPECT_NE(tiles[earlier], tile)
            << "tile (" << tx << ", " << ty << ") repeats an earlier tile";
      }
      tiles.push_back(std::move(tile));
    }
  }
  ASSERT_EQ(tiles.size(), static_cast<size_t>((kWidth / kTile) * (kHeight / kTile)));
}

TEST(NullRegistration, RefusesEverythingRatherThanPretending) {
  // Kept beside the real one so the pair is visible: the null engine is what a WASM build gets
  // (ADR 0052), and it refuses rather than returning an identity that would look like a stitch.
  NullRegistrationEngine engine;
  EXPECT_FALSE(engine.ExtractFeatures(FrameRef{}).ok());
}

}  // namespace
}  // namespace sphanorama

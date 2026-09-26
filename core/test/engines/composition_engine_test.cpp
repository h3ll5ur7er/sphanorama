// The preview: every direction of the sphere coloured from the frame that looks at it most
// squarely. What is knowable without eyes is where a direction lands and which frame it comes
// from, so that is what these assert; whether the result is a good picture is the round trip
// against a rendered dataset's own panorama, in composition_accuracy_test.cpp.
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <vector>

#include "engines/composition_engine/nearest_centre_composition_engine.h"
#include "resource_access/frame_store_access/memory_frame_store_access.h"
#include "utilities/camera_model.h"
#include "utilities/equirect.h"
#include "utilities/quaternion.h"

namespace sphanorama {
namespace {

using Rgb = std::array<uint8_t, 3>;

// Counts pins per frame, so a test can say every pin was released on every path; fills what it
// allocates with garbage; and can refuse to pin what it allocates, which is the one refusal the
// memory store cannot be made to give.
class PinCountingStore final : public IFrameStoreAccess {
 public:
  bool refusePinOfAllocated = false;
  explicit PinCountingStore(IFrameStoreAccess& real) : real_(real) {}
  int32_t outstanding() const {
    int32_t total = 0;
    for (const auto& [id, count] : pins_) total += count;
    return total;
  }
  Result<FrameStoreBudget> Budget() override { return real_.Budget(); }
  Result<FrameRef> Allocate(int32_t w, int32_t h, PixelFormat f) override {
    Result<FrameRef> allocated = real_.Allocate(w, h, f);
    if (!allocated.ok()) return allocated;
    allocated_.push_back(allocated.value.buffer.value);
    // The contract does not promise zeroed bytes, and the memory store happens to give them, so
    // an engine relying on that would pass here and paint garbage on a store that does not.
    Result<std::span<uint8_t>> bytes = real_.Pin(allocated.value);
    if (bytes.ok()) {
      std::fill(bytes.value.begin(), bytes.value.end(), uint8_t{0xA5});
      (void)real_.Release(allocated.value);
    }
    return allocated;
  }
  Result<std::span<uint8_t>> Pin(const FrameRef& frame) override {
    if (refusePinOfAllocated &&
        std::find(allocated_.begin(), allocated_.end(), frame.buffer.value) != allocated_.end()) {
      return Err<std::span<uint8_t>>(StatusCode::Internal, "PinCountingStore", "refused");
    }
    Result<std::span<uint8_t>> pinned = real_.Pin(frame);
    if (pinned.ok()) ++pins_[frame.buffer.value];
    return pinned;
  }
  Status Release(const FrameRef& frame) override {
    Status released = real_.Release(frame);
    if (released.ok()) --pins_[frame.buffer.value];
    return released;
  }
  Result<Residency> ResidencyOf(const FrameRef& f) override { return real_.ResidencyOf(f); }
  Status Demote(const FrameRef& f, Residency t) override { return real_.Demote(f, t); }
  Status Adopt(const FrameRef& f) override { return real_.Adopt(f); }
  Status Forget(const FrameRef& f) override { return real_.Forget(f); }
  Status Clear() override { return real_.Clear(); }
  Result<uint64_t> TierGeneration() override { return real_.TierGeneration(); }
  Result<uint64_t> ContentHash(const FrameRef& f) override { return real_.ContentHash(f); }

 private:
  IFrameStoreAccess& real_;
  std::map<uint64_t, int32_t> pins_;
  std::vector<uint64_t> allocated_;
};

constexpr int32_t kWidth = 64;
constexpr int32_t kHeight = 48;

class NearestCentreComposition : public ::testing::Test {
 protected:
  MemoryFrameStoreAccess real{1 << 26};
  PinCountingStore store{real};
  NearestCentreCompositionEngine engine{store};
  const Intrinsics lens = LensFromFieldOfView(60.0, 46.8, kWidth, kHeight);
  GlobalSolution solution;
  std::vector<FrameRef> frames;

  void SetUp() override { solution.intrinsics = lens; }

  template <typename Painter>
  FrameRef Paint(PixelFormat format, Painter paint, int32_t width = kWidth,
                 int32_t height = kHeight) {
    Result<FrameRef> allocated = real.Allocate(width, height, format);
    EXPECT_TRUE(allocated.ok()) << allocated.status.detail;
    Result<std::span<uint8_t>> pinned = real.Pin(allocated.value);
    EXPECT_TRUE(pinned.ok()) << pinned.status.detail;
    for (int32_t y = 0; y < height; ++y) {
      for (int32_t x = 0; x < width; ++x) {
        const Rgb rgb = paint(x, y);
        const size_t channels = format == PixelFormat::Gray8 ? 1 : 4;
        const size_t at = (static_cast<size_t>(y) * static_cast<size_t>(allocated.value.stride)) +
                          static_cast<size_t>(x) * channels;
        pinned.value[at] = rgb[0];
        if (channels == 1) continue;
        pinned.value[at + 1] = rgb[1];
        pinned.value[at + 2] = rgb[2];
        pinned.value[at + 3] = 255;
      }
    }
    EXPECT_TRUE(real.Release(allocated.value).ok());
    return allocated.value;
  }

  // A frame looking along `rotation`, of one colour or painted by `paint`.
  template <typename P>
  void Look(const Quat& rotation, P paint) {
    const FrameRef frame = Paint(PixelFormat::RGBA8, paint);
    frames.push_back(frame);
    solution.frames.push_back(frame.id);
    solution.rotations.push_back(rotation);
  }
  void Look(const Quat& rotation, Rgb colour) {
    Look(rotation, [colour](int32_t, int32_t) { return colour; });
  }

  struct Preview {
    int32_t width = 0, height = 0;
    std::vector<uint8_t> bytes;
    std::array<uint8_t, 4> At(int32_t x, int32_t y) const {
      if (x < 0 || y < 0 || x >= width || y >= height) {
        ADD_FAILURE() << "(" << x << ", " << y << ") is outside a " << width << "x" << height
                      << " preview";
        return {};
      }
      const size_t at = (static_cast<size_t>(y) * static_cast<size_t>(width) +
                         static_cast<size_t>(x)) * 4;
      return {bytes[at], bytes[at + 1], bytes[at + 2], bytes[at + 3]};
    }
  };

  // Renders, copies the answer out and forgets it, which is the caller's to do.
  Preview Render(int32_t maxWidth, const GainMap& gains = {}) {
    Result<FrameRef> answer = engine.RenderPreview(solution, frames, gains, maxWidth);
    EXPECT_TRUE(answer.ok()) << answer.status.detail;
    if (!answer.ok()) return {};
    EXPECT_EQ(answer.value.format, PixelFormat::RGBA8);
    EXPECT_EQ(store.outstanding(), 0) << "every pin is released";
    Result<std::span<uint8_t>> pinned = real.Pin(answer.value);
    EXPECT_TRUE(pinned.ok());
    Preview preview{answer.value.width, answer.value.height, {}};
    for (int32_t y = 0; y < preview.height; ++y) {
      const uint8_t* row = pinned.value.data() + static_cast<size_t>(y) *
                                                     static_cast<size_t>(answer.value.stride);
      preview.bytes.insert(preview.bytes.end(), row, row + static_cast<size_t>(preview.width) * 4);
    }
    EXPECT_TRUE(real.Release(answer.value).ok());
    EXPECT_TRUE(real.Forget(answer.value).ok()) << "the answer is the caller's to forget";
    return preview;
  }

  StatusCode Refused(int32_t maxWidth = 64, const GainMap& gains = {}) {
    Result<FrameRef> answer = engine.RenderPreview(solution, frames, gains, maxWidth);
    EXPECT_EQ(store.outstanding(), 0) << "every pin is released on a refusal too";
    if (answer.ok()) (void)real.Forget(answer.value);
    return answer.status.code;
  }
};

TEST_F(NearestCentreComposition, IsTwiceAsWideAsHighAndNoWiderThanAsked) {
  Look(Quat{}, Rgb{10, 20, 30});
  const Preview even = Render(256);
  EXPECT_EQ(even.width, 256);
  EXPECT_EQ(even.height, 128);
  const Preview odd = Render(257);
  EXPECT_EQ(odd.width, 256);
  EXPECT_EQ(odd.height, 128);
  const Preview smallest = Render(2);
  EXPECT_EQ(smallest.width, 2);
  EXPECT_EQ(smallest.height, 1);
}

// Forward is the centre of the panorama, and only what the frame sees is covered: alpha is
// coverage, and a direction no frame sees is transparent black rather than a colour.
TEST_F(NearestCentreComposition, AFrameLookingForwardCoversTheCentreAndNothingBehind) {
  Look(Quat{}, Rgb{10, 20, 30});
  const Preview preview = Render(256);
  EXPECT_EQ(preview.At(128, 64), (std::array<uint8_t, 4>{10, 20, 30, 255}));
  EXPECT_EQ(preview.At(127, 63), (std::array<uint8_t, 4>{10, 20, 30, 255}));
  EXPECT_EQ(preview.At(0, 64), (std::array<uint8_t, 4>{0, 0, 0, 0}));
  EXPECT_EQ(preview.At(128, 0), (std::array<uint8_t, 4>{0, 0, 0, 0}));

  // Sixty degrees of a 256-wide panorama is 42.7 columns along the equator.
  int32_t covered = 0;
  for (int32_t x = 0; x < preview.width; ++x) covered += preview.At(x, 64)[3] == 255 ? 1 : 0;
  EXPECT_NEAR(covered, 256.0 * 60.0 / 360.0, 1.5);
}

// Turned a quarter to the left, the frame lands a quarter of the way across: `FromAzimuthElevation`
// turns toward -X, and longitude increases toward +X.
TEST_F(NearestCentreComposition, ARotationMovesWhereTheFrameLands) {
  Look(FromAzimuthElevation(90.0, 0.0), Rgb{10, 20, 30});
  const Preview preview = Render(256);
  EXPECT_EQ(preview.At(64, 64)[3], 255);
  EXPECT_EQ(preview.At(128, 64)[3], 0);
  EXPECT_EQ(preview.At(192, 64)[3], 0);
}

// The colour at a direction is the frame's colour where that direction lands through the lens,
// interpolated between pixel centres: a frame whose red is its column shows, at each covered
// direction, the column the lens puts it at less the half pixel to that column's centre.
TEST_F(NearestCentreComposition, TheColourIsTheFramesWhereTheDirectionLands) {
  const Quat turned = FromAzimuthElevation(10.0, 5.0);
  Look(turned, [](int32_t x, int32_t y) {
    return Rgb{static_cast<uint8_t>(x * 3), static_cast<uint8_t>(y * 4), 7};
  });
  const Preview preview = Render(512);
  int32_t checked = 0;
  for (int32_t y = 0; y < preview.height; y += 7) {
    for (int32_t x = 0; x < preview.width; x += 5) {
      const std::optional<Vec3> world = EquirectDirection(Pixel{x + 0.5, y + 0.5}, 512, 256);
      ASSERT_TRUE(world.has_value());
      const ProjectedPixel at = Project(lens, Rotate(Conjugate(turned), *world));
      const bool inside = at.valid && at.pixel.x >= 0.5 && at.pixel.x <= kWidth - 0.5 &&
                          at.pixel.y >= 0.5 && at.pixel.y <= kHeight - 0.5;
      if (!inside) continue;
      const std::array<uint8_t, 4> got = preview.At(x, y);
      ASSERT_EQ(got[3], 255) << x << " " << y;
      EXPECT_NEAR(got[0], (at.pixel.x - 0.5) * 3.0, 0.51) << x << " " << y;
      EXPECT_NEAR(got[1], (at.pixel.y - 0.5) * 4.0, 0.51) << x << " " << y;
      EXPECT_EQ(got[2], 7);
      ++checked;
    }
  }
  EXPECT_GT(checked, 100) << "the premise: the frame covers some of the sampled directions";
}

// Where two frames see a direction, the one looking at it most squarely colours it: two frames
// twenty degrees either side of forward overlap across the middle forty, and meet at forward.
TEST_F(NearestCentreComposition, WhereFramesOverlapTheOneLookingMostSquarelyWins) {
  Look(FromAzimuthElevation(20.0, 0.0), Rgb{200, 0, 0});   // toward -X: the left of the panorama
  Look(FromAzimuthElevation(-20.0, 0.0), Rgb{0, 0, 200});  // toward +X: the right
  const Preview preview = Render(360);
  // One degree a column: forward sits between columns 179 and 180.
  EXPECT_EQ(preview.At(170, 90), (std::array<uint8_t, 4>{200, 0, 0, 255}));
  EXPECT_EQ(preview.At(179, 90), (std::array<uint8_t, 4>{200, 0, 0, 255}));
  EXPECT_EQ(preview.At(180, 90), (std::array<uint8_t, 4>{0, 0, 200, 255}));
  EXPECT_EQ(preview.At(190, 90), (std::array<uint8_t, 4>{0, 0, 200, 255}));
}

TEST_F(NearestCentreComposition, AGainScalesItsFramesColourAndSaturates) {
  Look(Quat{}, Rgb{200, 100, 10});
  GainMap gains;
  gains.frames = solution.frames;
  gains.perFrameGain = {0.5};
  EXPECT_EQ(Render(256, gains).At(128, 64), (std::array<uint8_t, 4>{100, 50, 5, 255}));
  gains.perFrameGain = {2.0};
  EXPECT_EQ(Render(256, gains).At(128, 64), (std::array<uint8_t, 4>{255, 200, 20, 255}));
}

// A solution that placed nothing is a sphere nothing sees, not a refusal.
TEST_F(NearestCentreComposition, NothingPlacedIsNothingCovered) {
  const Preview preview = Render(64);
  ASSERT_EQ(preview.width, 64);
  for (size_t i = 3; i < preview.bytes.size(); i += 4) ASSERT_EQ(preview.bytes[i], 0) << i;
}

TEST_F(NearestCentreComposition, InputThatIsNotAPreviewIsRefused) {
  Look(Quat{}, Rgb{1, 2, 3});
  Look(FromAzimuthElevation(30.0, 0.0), Rgb{4, 5, 6});
  EXPECT_EQ(Refused(1), StatusCode::InvalidArgument);
  EXPECT_EQ(Refused(0), StatusCode::InvalidArgument);
  EXPECT_EQ(Refused(-4), StatusCode::InvalidArgument);

  const std::vector<FrameRef> all = frames;
  frames.pop_back();
  EXPECT_EQ(Refused(), StatusCode::InvalidArgument) << "fewer frames than the solution names";
  frames = {all[1], all[0]};
  EXPECT_EQ(Refused(), StatusCode::InvalidArgument) << "the solution's frames in another order";
  frames = all;

  const GlobalSolution good = solution;
  solution.rotations[1] = Quat{0.0, 0.0, 0.0, 0.0};
  EXPECT_EQ(Refused(), StatusCode::InvalidArgument) << "a rotation that is not one";
  solution = good;
  solution.rotations.pop_back();
  EXPECT_EQ(Refused(), StatusCode::InvalidArgument) << "rotations not parallel to the frames";
  solution = good;
  solution.intrinsics.fx = 0.0;
  EXPECT_EQ(Refused(), StatusCode::InvalidArgument) << "a lens that cannot project";
  solution = good;

  GainMap gains;
  gains.frames = solution.frames;
  gains.perFrameGain = {1.0};
  EXPECT_EQ(Refused(64, gains), StatusCode::InvalidArgument) << "a gain short";
  gains.perFrameGain = {1.0, std::numeric_limits<double>::quiet_NaN()};
  EXPECT_EQ(Refused(64, gains), StatusCode::InvalidArgument) << "a gain not a figure";
  gains.perFrameGain = {1.0, 0.0};
  EXPECT_EQ(Refused(64, gains), StatusCode::InvalidArgument) << "a gain of zero";
  gains.perFrameGain = {1.0, -1.0};
  EXPECT_EQ(Refused(64, gains), StatusCode::InvalidArgument) << "a gain below zero";
  gains.perFrameGain = {1.0, std::numeric_limits<double>::infinity()};
  EXPECT_EQ(Refused(64, gains), StatusCode::InvalidArgument) << "an infinite gain";
  gains.perFrameGain = {1.0, 1.0};
  gains.frames = {solution.frames[1], solution.frames[0]};
  EXPECT_EQ(Refused(64, gains), StatusCode::InvalidArgument) << "gains naming another order";
  gains.frames = solution.frames;
  ASSERT_EQ(Refused(64, gains), StatusCode::Ok) << "the premise: those gains are otherwise fine";

  frames[1] = Paint(PixelFormat::RGBA8, [](int32_t, int32_t) { return Rgb{}; }, kWidth * 2,
                    kHeight * 2);
  solution.frames[1] = frames[1].id;
  EXPECT_EQ(Refused(), StatusCode::InvalidArgument) << "a frame another size than the lens";
  frames[1] = Paint(PixelFormat::Gray8, [](int32_t, int32_t) { return Rgb{}; });
  solution.frames[1] = frames[1].id;
  EXPECT_EQ(Refused(), StatusCode::Unsupported) << "a frame that is not RGBA8";
}

// A frame is a value the caller passes in, and the store's allocation is the truth: a handle whose
// stride or height claims more than the store holds is refused rather than read past.
TEST_F(NearestCentreComposition, AHandleClaimingMoreThanTheStoreHoldsIsRefused) {
  Look(Quat{}, Rgb{1, 2, 3});
  frames[0].stride = kWidth * 4 * 2;
  EXPECT_EQ(Refused(), StatusCode::InvalidArgument) << "a stride past the allocation";
  frames[0].stride = kWidth * 4 - 4;
  EXPECT_EQ(Refused(), StatusCode::InvalidArgument) << "a stride shorter than a row";
}

// The store's own status, whole, when a frame cannot be pinned.
TEST_F(NearestCentreComposition, AFrameTheStoreDoesNotHoldIsTheStoresRefusal) {
  Look(Quat{}, Rgb{1, 2, 3});
  Look(FromAzimuthElevation(30.0, 0.0), Rgb{4, 5, 6});
  ASSERT_TRUE(real.Forget(frames[1]).ok());
  Result<FrameRef> answer = engine.RenderPreview(solution, frames, GainMap{}, 64);
  EXPECT_EQ(answer.status.code, StatusCode::NotFound);
  EXPECT_EQ(store.outstanding(), 0) << "the first frame's pin is released";
}

// A preview the store has no room for is the store's refusal, with the frames' pins released.
TEST_F(NearestCentreComposition, AnAnswerTheStoreCannotHoldIsTheStoresRefusal) {
  Look(Quat{}, Rgb{1, 2, 3});
  const int64_t before = real.Budget().value.heapUsedBytes;
  EXPECT_EQ(Refused(8192), StatusCode::FrameStoreExhausted) << "8192x4096 RGBA8 is 128 MiB";
  EXPECT_EQ(real.Budget().value.heapUsedBytes, before);
}

// And one it allocated but could not pin is forgotten rather than left to account for its bytes.
TEST_F(NearestCentreComposition, AnAnswerThatCannotBePinnedIsForgotten) {
  Look(Quat{}, Rgb{1, 2, 3});
  const int64_t before = real.Budget().value.heapUsedBytes;
  store.refusePinOfAllocated = true;
  EXPECT_EQ(Refused(64), StatusCode::Internal) << "the store's own refusal";
  EXPECT_EQ(real.Budget().value.heapUsedBytes, before) << "nothing is left accounted for";
}

// The other methods are later increments, and say so.
TEST_F(NearestCentreComposition, WhatIsNotBuiltSaysSo) {
  EXPECT_EQ(engine.CompensateExposure(solution, frames).status.code, StatusCode::Unsupported);
  EXPECT_EQ(engine.DetectGhosts(solution, {}).status.code, StatusCode::Unsupported);
  EXPECT_EQ(engine.FindSeams(solution, frames, {}, {}, {}).status.code, StatusCode::Unsupported);
  EXPECT_EQ(engine.BlendTile(solution, frames, {}, {}, {}, 0, 0).status.code,
            StatusCode::Unsupported);
}

}  // namespace
}  // namespace sphanorama

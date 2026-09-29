// The preview: every direction of the sphere coloured from the frame that looks at it most
// squarely. What is knowable without eyes is where a direction lands and which frame it comes
// from, so that is what these assert; whether the result is a good picture is the round trip
// against a rendered dataset's own panorama, in composition_accuracy_test.cpp.
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "engines/composition_engine/nearest_centre_composition_engine.h"
#include "resource_access/frame_store_access/memory_frame_store_access.h"
#include "support/fake_spill_sink.h"
#include "utilities/camera_model.h"
#include "utilities/equirect.h"
#include "utilities/quaternion.h"

namespace sphanorama {
namespace {

using Rgb = std::array<uint8_t, 3>;

// Counts pins per frame, so a test can say every pin was released on every path; fills what it
// allocates with garbage; and can give the refusals and the handles the memory store never does —
// a pin, a release or a forget declined, and an allocation described as another shape than it is.
class PinCountingStore final : public IFrameStoreAccess {
 public:
  enum class Whose { Nobody, Allocated, HandedIn };
  // A code of its own for each refusal, none of them one the engine writes itself, so a test can
  // tell the store's code passed through from the engine's own.
  static constexpr StatusCode kPinRefused = StatusCode::CodecFailure;
  static constexpr StatusCode kReleaseDeclined = StatusCode::FailedPrecondition;
  static constexpr StatusCode kTierUnknown = StatusCode::Cancelled;
  static constexpr StatusCode kForgetDeclined = StatusCode::StorageQuotaExceeded;
  bool refusePinOfAllocated = false;
  bool refusePinOfHandedIn = false;
  // Allocates this many pixels more a row than asked and says so only through the stride: a real
  // padded allocation, where `describeAllocated` can only relabel one.
  int32_t padAllocatedPixels = 0;
  // Pins a span this many bytes short of the store's allocation: a padded layout whose span ends
  // where the last row ends, before that row's padding.
  int64_t trimTailOfAllocated = 0;
  int64_t trimTailOfHandedIn = 0;
  bool refuseForgetOfAllocated = false;
  bool refuseResidency = false;
  // Refused once each, so the refusal is the call under test's to report rather than permanent.
  Whose refuseRelease = Whose::Nobody;
  int releaseRefusals = 1;
  // When valid, only this frame's release is declined, so a refusal can be told which one it was.
  FrameId refuseReleaseOnlyOf;
  // Applied to the handle `Allocate` answers with; the allocation itself is the real store's.
  std::function<void(FrameRef&)> describeAllocated;
  explicit PinCountingStore(IFrameStoreAccess& real) : real_(real) {}
  int32_t outstanding() const {
    int32_t total = 0;
    for (const auto& [id, count] : pins_) total += count;
    return total;
  }
  Result<FrameStoreBudget> Budget() override { return real_.Budget(); }
  Result<FrameRef> Allocate(int32_t w, int32_t h, PixelFormat f) override {
    Result<FrameRef> allocated = real_.Allocate(w + padAllocatedPixels, h, f);
    if (!allocated.ok()) return allocated;
    allocated.value.width = w;
    allocated_.push_back(allocated.value.buffer.value);
    // The contract does not promise zeroed bytes, and the memory store happens to give them, so
    // an engine relying on that would pass here and paint garbage on a store that does not.
    Result<std::span<uint8_t>> bytes = real_.Pin(allocated.value);
    if (bytes.ok()) {
      std::fill(bytes.value.begin(), bytes.value.end(), uint8_t{0xA5});
      (void)real_.Release(allocated.value);
    }
    if (describeAllocated) describeAllocated(allocated.value);
    return allocated;
  }
  bool Allocated(const FrameRef& frame) const {
    return std::find(allocated_.begin(), allocated_.end(), frame.buffer.value) != allocated_.end();
  }
  Result<std::span<uint8_t>> Pin(const FrameRef& frame) override {
    if ((refusePinOfAllocated && Allocated(frame)) || (refusePinOfHandedIn && !Allocated(frame))) {
      return Err<std::span<uint8_t>>(kPinRefused, "PinCountingStore", "refused");
    }
    Result<std::span<uint8_t>> pinned = real_.Pin(frame);
    if (pinned.ok()) {
      ++pins_[frame.buffer.value];
      ++pinnedEver_[frame.buffer.value];
      const int64_t trim = Allocated(frame) ? trimTailOfAllocated : trimTailOfHandedIn;
      pinned.value = pinned.value.first(pinned.value.size() - static_cast<size_t>(trim));
    }
    return pinned;
  }
  int32_t PinnedEver(const FrameRef& frame) const {
    const auto found = pinnedEver_.find(frame.buffer.value);
    return found == pinnedEver_.end() ? 0 : found->second;
  }
  Status Release(const FrameRef& frame) override {
    const Whose whose = Allocated(frame) ? Whose::Allocated : Whose::HandedIn;
    const bool named = !refuseReleaseOnlyOf.valid() || frame.id == refuseReleaseOnlyOf;
    if (refuseRelease == whose && named && releaseRefusals > 0) {
      --releaseRefusals;
      return Fail(kReleaseDeclined, "PinCountingStore", "declined to release");
    }
    Status released = real_.Release(frame);
    if (released.ok()) --pins_[frame.buffer.value];
    return released;
  }
  Result<Residency> ResidencyOf(const FrameRef& f) override {
    if (refuseResidency) return Err<Residency>(kTierUnknown, "PinCountingStore", "unsure");
    return real_.ResidencyOf(f);
  }
  Status Demote(const FrameRef& f, Residency t) override { return real_.Demote(f, t); }
  Status Adopt(const FrameRef& f) override { return real_.Adopt(f); }
  Status Forget(const FrameRef& f) override {
    if (refuseForgetOfAllocated && Allocated(f)) {
      return Fail(kForgetDeclined, "PinCountingStore", "declined to forget");
    }
    return real_.Forget(f);
  }
  Status Clear() override { return real_.Clear(); }
  Result<uint64_t> TierGeneration() override { return real_.TierGeneration(); }
  Result<uint64_t> ContentHash(const FrameRef& f) override { return real_.ContentHash(f); }

 private:
  IFrameStoreAccess& real_;
  std::map<uint64_t, int32_t> pins_;
  std::map<uint64_t, int32_t> pinnedEver_;
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

  Status Refusal(int32_t maxWidth = 64, const GainMap& gains = {}) {
    Result<FrameRef> answer = engine.RenderPreview(solution, frames, gains, maxWidth);
    EXPECT_EQ(store.outstanding(), 0) << "every pin is released on a refusal too";
    if (answer.ok()) (void)real.Forget(answer.value);
    return answer.status;
  }
  // What the contract tells a caller handed a preview back: release it if the store still has it
  // pinned, then forget it.
  // What it hands back is the preview it allocated, not a frame of the caller's, which would be
  // forgotten here in the preview's place and pass for it.
  void CleanUpHandedBack(const FrameRef& preview) {
    ASSERT_TRUE(store.Allocated(preview)) << "the call's own allocation";
    for (const FrameRef& frame : frames) ASSERT_FALSE(frame.id == preview.id);
    Result<Residency> tier = store.ResidencyOf(preview);
    ASSERT_TRUE(tier.ok());
    if (tier.value == Residency::HeapPinned) {
      EXPECT_TRUE(store.Release(preview).ok());
    }
    EXPECT_TRUE(store.Forget(preview).ok());
  }
  StatusCode Refused(int32_t maxWidth = 64, const GainMap& gains = {}) {
    return Refusal(maxWidth, gains).code;
  }
  // The engine's own refusal, by the phrase only its guard writes: a store refusing the same input
  // later with the same code would otherwise pass for it.
  ::testing::AssertionResult RefusedSaying(const std::string& phrase, int32_t maxWidth = 64,
                                           const GainMap& gains = {}) {
    const Status status = Refusal(maxWidth, gains);
    if (status.code == StatusCode::InvalidArgument &&
        status.detail.find(phrase) != std::string::npos) {
      return ::testing::AssertionSuccess();
    }
    return ::testing::AssertionFailure() << "refused with " << static_cast<int>(status.code)
                                         << ": " << status.detail;
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

  // Sixty degrees of a 256-wide panorama is 42.7 columns along the equator, and 46.8 degrees of
  // its 128 rows is 33.3 down the middle: each counts both of that axis's edges.
  int32_t covered = 0;
  for (int32_t x = 0; x < preview.width; ++x) covered += preview.At(x, 64)[3] == 255 ? 1 : 0;
  EXPECT_NEAR(covered, 256.0 * 60.0 / 360.0, 1.5);
  int32_t tall = 0;
  for (int32_t y = 0; y < preview.height; ++y) tall += preview.At(128, y)[3] == 255 ? 1 : 0;
  EXPECT_NEAR(tall, 128.0 * 46.8 / 180.0, 1.5);
}

// Where a frame's image ends is where it stops covering: a direction the lens puts half a source
// pixel past any edge is left uncovered, and one half a pixel inside it is covered.
TEST_F(NearestCentreComposition, AFrameCoversUpToItsEdgeAndNoFurther) {
  constexpr int32_t kPreview = 256;
  const Vec3 centre = *EquirectDirection(Pixel{128.5, 64.5}, kPreview, kPreview / 2);
  // The turn — about the vertical for a column, in elevation for a row — that lands the preview's
  // centre pixel at `target` of the frame, found by bisection on the projection itself, so it
  // holds for whatever lens the fixture has.
  const auto turnLanding = [&](double target, bool row) {
    const auto turn = [&](double degrees) {
      return row ? FromAzimuthElevation(0.0, degrees) : FromAzimuthElevation(degrees, 0.0);
    };
    const auto landed = [&](double degrees) {
      const Pixel at = Project(lens, Rotate(Conjugate(turn(degrees)), centre)).pixel;
      return row ? at.y : at.x;
    };
    double lo = -45.0, hi = 45.0;
    const bool rising = landed(hi) > landed(lo);
    for (int k = 0; k < 100; ++k) {
      const double mid = 0.5 * (lo + hi);
      ((landed(mid) < target) == rising ? lo : hi) = mid;
    }
    return turn(0.5 * (lo + hi));
  };
  struct Case {
    double target;
    bool row;
    uint8_t alpha;
  };
  const std::vector<Case> cases = {
      {-0.5, false, 0},  {0.5, false, 255},  {kWidth - 0.5, false, 255},  {kWidth + 0.5, false, 0},
      {-0.5, true, 0},   {0.5, true, 255},   {kHeight - 0.5, true, 255},  {kHeight + 0.5, true, 0}};
  for (const Case& c : cases) {
    frames.clear();
    solution.frames.clear();
    solution.rotations.clear();
    Look(turnLanding(c.target, c.row), Rgb{10, 20, 30});
    EXPECT_EQ(Render(kPreview).At(128, 64)[3], c.alpha)
        << "landing at " << (c.row ? "row " : "column ") << c.target;
  }
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
// direction, the column the lens puts it at less the half pixel to that column's centre — and in
// the half pixel between the outermost centres and the frame's edge, the outermost pixel.
TEST_F(NearestCentreComposition, TheColourIsTheFramesWhereTheDirectionLands) {
  const Quat turned = FromAzimuthElevation(10.0, 5.0);
  Look(turned, [](int32_t x, int32_t y) {
    return Rgb{static_cast<uint8_t>(x * 3), static_cast<uint8_t>(y * 4), 7};
  });
  const Preview preview = Render(512);
  int32_t checked = 0;
  int32_t atTheEdge = 0;
  for (int32_t y = 0; y < preview.height; ++y) {
    for (int32_t x = 0; x < preview.width; ++x) {
      const std::optional<Vec3> world = EquirectDirection(Pixel{x + 0.5, y + 0.5}, 512, 256);
      ASSERT_TRUE(world.has_value());
      const ProjectedPixel at = Project(lens, Rotate(Conjugate(turned), *world));
      const bool inside = at.valid && at.pixel.x >= 0.0 && at.pixel.x <= kWidth &&
                          at.pixel.y >= 0.0 && at.pixel.y <= kHeight;
      if (!inside) continue;
      const double column = std::clamp(at.pixel.x - 0.5, 0.0, kWidth - 1.0);
      const double row = std::clamp(at.pixel.y - 0.5, 0.0, kHeight - 1.0);
      if (column != at.pixel.x - 0.5 || row != at.pixel.y - 0.5) ++atTheEdge;
      const std::array<uint8_t, 4> got = preview.At(x, y);
      ASSERT_EQ(got[3], 255) << x << " " << y;
      EXPECT_NEAR(got[0], column * 3.0, 0.51) << x << " " << y;
      EXPECT_NEAR(got[1], row * 4.0, 0.51) << x << " " << y;
      EXPECT_EQ(got[2], 7);
      ++checked;
    }
  }
  EXPECT_GT(checked, 100) << "the premise: the frame covers some of the sampled directions";
  EXPECT_GT(atTheEdge, 10) << "the premise: some of them land in the half pixel at the edge";
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

// Each frame's gain is its own: two frames of one colour, each drawn at its own gain.
TEST_F(NearestCentreComposition, EachFrameIsScaledByItsOwnGain) {
  Look(Quat{}, Rgb{100, 100, 100});
  Look(FromAzimuthElevation(90.0, 0.0), Rgb{100, 100, 100});
  GainMap gains;
  gains.frames = solution.frames;
  gains.perFrameGain = {0.5, 2.0};
  const Preview preview = Render(256, gains);
  EXPECT_EQ(preview.At(128, 64), (std::array<uint8_t, 4>{50, 50, 50, 255})) << "frame 0";
  EXPECT_EQ(preview.At(64, 64), (std::array<uint8_t, 4>{200, 200, 200, 255})) << "frame 1";
}

// A solution that placed nothing is a sphere nothing sees, not a refusal.
// Whatever its lens, since a default solution has none and there is nothing to project through it.
TEST_F(NearestCentreComposition, NothingPlacedIsNothingCovered) {
  solution.intrinsics = Intrinsics{};
  ASSERT_FALSE(IsUsableLens(solution.intrinsics)) << "the premise";
  const Preview preview = Render(64);
  ASSERT_EQ(preview.width, 64);
  for (size_t i = 3; i < preview.bytes.size(); i += 4) ASSERT_EQ(preview.bytes[i], 0) << i;
}

TEST_F(NearestCentreComposition, InputThatIsNotAPreviewIsRefused) {
  Look(Quat{}, Rgb{1, 2, 3});
  Look(FromAzimuthElevation(30.0, 0.0), Rgb{4, 5, 6});
  EXPECT_TRUE(RefusedSaying("no half to be high", 1));
  EXPECT_TRUE(RefusedSaying("no half to be high", 0));
  EXPECT_TRUE(RefusedSaying("no half to be high", -4));

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
  const std::string unnamed = "do not name the solution's frames";
  gains.frames = solution.frames;
  gains.perFrameGain = {1.0};
  EXPECT_TRUE(RefusedSaying(unnamed, 64, gains)) << "a gain short";
  gains.perFrameGain = {1.0, 1.0, 1.0};
  EXPECT_TRUE(RefusedSaying(unnamed, 64, gains)) << "a gain too many";
  gains.perFrameGain = {};
  EXPECT_TRUE(RefusedSaying(unnamed, 64, gains)) << "frames named and no gains";
  gains.perFrameGain = {1.0, 1.0};
  gains.frames = {};
  EXPECT_TRUE(RefusedSaying(unnamed, 64, gains)) << "gains and no frames named";
  gains.frames = {solution.frames[0], solution.frames[1], solution.frames[1]};
  EXPECT_TRUE(RefusedSaying(unnamed, 64, gains)) << "a frame named too many";
  gains.frames = solution.frames;
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

  // Each dimension on its own, so neither half of the check stands in for the other.
  frames[1] = Paint(PixelFormat::RGBA8, [](int32_t, int32_t) { return Rgb{}; }, kWidth * 2);
  solution.frames[1] = frames[1].id;
  EXPECT_TRUE(RefusedSaying("another size than the lens")) << "a frame another width";
  frames[1] = Paint(PixelFormat::RGBA8, [](int32_t, int32_t) { return Rgb{}; }, kWidth,
                    kHeight * 2);
  solution.frames[1] = frames[1].id;
  EXPECT_TRUE(RefusedSaying("another size than the lens")) << "a frame another height";
  frames[1] = Paint(PixelFormat::Gray8, [](int32_t, int32_t) { return Rgb{}; });
  solution.frames[1] = frames[1].id;
  EXPECT_EQ(Refused(), StatusCode::Unsupported) << "a frame that is not RGBA8";
}

// Each preview pixel names its frame in two bytes, so a solution that names more frames than that
// can count is refused before anything is pinned or allocated.
TEST_F(NearestCentreComposition, MoreFramesThanAPreviewCanTellApartIsRefused) {
  const FrameRef frame = Paint(PixelFormat::RGBA8, [](int32_t, int32_t) { return Rgb{}; });
  frames.assign(65536, frame);
  solution.frames.assign(65536, frame.id);
  solution.rotations.assign(65536, Quat{});
  EXPECT_TRUE(RefusedSaying("more frames than a preview can tell apart"));
  frames.resize(65535);
  solution.frames.resize(65535);
  solution.rotations.resize(65535);
  EXPECT_EQ(Refused(2), StatusCode::Ok) << "one fewer is a preview: the last index is 65,534";
}

// A frame is a value the caller passes in, and the store's allocation is the truth: a handle whose
// stride or height claims more than the store holds is refused rather than read past.
TEST_F(NearestCentreComposition, AHandleClaimingMoreThanTheStoreHoldsIsRefused) {
  Look(Quat{}, Rgb{1, 2, 3});
  frames[0].stride = kWidth * 4 * 2;
  EXPECT_EQ(Refused(), StatusCode::InvalidArgument) << "a stride past the allocation";
  frames[0].stride = kWidth * 4 - 4;
  EXPECT_EQ(Refused(), StatusCode::InvalidArgument) << "a stride shorter than a row";
  frames[0].stride = kWidth * 4 + 4;
  EXPECT_EQ(Refused(), StatusCode::InvalidArgument) << "a stride one pixel past the allocation";
}

// The store's own code for a frame it does not hold: it cannot say the frame's tier, so the frame is
// refused before it is pinned.
TEST_F(NearestCentreComposition, AFrameTheStoreDoesNotHoldIsTheStoresRefusal) {
  Look(Quat{}, Rgb{1, 2, 3});
  Look(FromAzimuthElevation(30.0, 0.0), Rgb{4, 5, 6});
  ASSERT_TRUE(real.Forget(frames[1]).ok());
  Result<FrameRef> answer = engine.RenderPreview(solution, frames, GainMap{}, 64);
  EXPECT_EQ(answer.status.code, StatusCode::NotFound);
  EXPECT_EQ(store.outstanding(), 0) << "the first frame's pin is released";
}

// A preview the store has no room for is the store's refusal, before any frame is pinned.
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
  EXPECT_EQ(Refused(64), PinCountingStore::kPinRefused) << "the store's own refusal";
  EXPECT_EQ(real.Budget().value.heapUsedBytes, before) << "nothing is left accounted for";
}

// A store may describe what it allocated differently from what was asked — padded rows, or fewer of
// them — and the preview is written through that description, so it is checked against the bytes
// before anything is written. Without the check a padded stride writes past the allocation.
TEST_F(NearestCentreComposition, AnAnswerOfAnotherShapeThanAskedIsRefusedAndForgotten) {
  Look(Quat{}, Rgb{1, 2, 3});
  const int64_t before = real.Budget().value.heapUsedBytes;
  const std::vector<std::pair<const char*, std::function<void(FrameRef&)>>> lies = {
      {"a stride padded past the allocation", [](FrameRef& f) { f.stride += 64; }},
      {"a stride padded one pixel past it", [](FrameRef& f) { f.stride += 4; }},
      {"a stride shorter than a row", [](FrameRef& f) { f.stride -= 4; }},
      {"another width", [](FrameRef& f) { f.width -= 2; }},
      {"another height", [](FrameRef& f) { f.height += 1; }},
      {"another format", [](FrameRef& f) { f.format = PixelFormat::Gray8; }},
  };
  for (const auto& [lie, describe] : lies) {
    store.describeAllocated = describe;
    EXPECT_FALSE(engine.RenderPreview(solution, frames, {}, 64).value.id.valid())
        << lie << ": a preview given back is not named";
    EXPECT_EQ(Refused(64), StatusCode::Internal) << lie;
    EXPECT_EQ(real.Budget().value.heapUsedBytes, before) << lie << ": the answer is forgotten";
  }
}

// A preview the store will not let go of is not an answer: its caller could not forget it.
TEST_F(NearestCentreComposition, AnAnswerTheStoreWillNotReleaseIsARefusalThatSaysSo) {
  Look(Quat{}, Rgb{1, 2, 3});
  store.refuseRelease = PinCountingStore::Whose::Allocated;
  Result<FrameRef> answer = engine.RenderPreview(solution, frames, {}, 64);
  ASSERT_FALSE(answer.ok());
  EXPECT_EQ(answer.status.code, PinCountingStore::kReleaseDeclined) << "the store's own code";
  EXPECT_NE(answer.status.detail.find("still pinned"), std::string::npos) << answer.status.detail;
  // And hands it back, since nothing else can name it to the store.
  ASSERT_TRUE(answer.value.id.valid()) << "the preview it could not give back";
  EXPECT_EQ(store.outstanding(), 1) << "pinned once, by the release that was declined";
  CleanUpHandedBack(answer.value);
  EXPECT_TRUE(real.Clear().ok()) << "nothing is left pinned";
}

// Nor is one produced while a frame the caller handed in stays pinned: the preview is forgotten and
// the release refused is what the caller hears about.
TEST_F(NearestCentreComposition, AFrameHandedInThatWillNotBeReleasedIsARefusal) {
  Look(Quat{}, Rgb{1, 2, 3});
  const int64_t before = real.Budget().value.heapUsedBytes;
  store.refuseRelease = PinCountingStore::Whose::HandedIn;
  Result<FrameRef> answer = engine.RenderPreview(solution, frames, {}, 64);
  EXPECT_EQ(answer.status.code, PinCountingStore::kReleaseDeclined) << "the store's own code";
  EXPECT_NE(answer.status.detail.find("still pinned"), std::string::npos) << answer.status.detail;
  EXPECT_FALSE(answer.value.id.valid()) << "the preview was given back, so none is named";
  EXPECT_EQ(real.Budget().value.heapUsedBytes, before) << "the preview is forgotten";
  // The state the refusal reports, exactly: the frame holds this call's pin, for its caller.
  EXPECT_EQ(store.outstanding(), 1);
  EXPECT_TRUE(store.Release(frames[0]).ok());
  EXPECT_FALSE(real.Release(frames[0]).ok()) << "that was the only pin";
}

// Where giving the answer back fails too, the refusal says the bytes are still charged.
TEST_F(NearestCentreComposition, AnAnswerThatCanBeNeitherPinnedNorForgottenSaysItIsStillCharged) {
  Look(Quat{}, Rgb{1, 2, 3});
  store.refusePinOfAllocated = true;
  store.refuseForgetOfAllocated = true;
  Result<FrameRef> answer = engine.RenderPreview(solution, frames, {}, 64);
  ASSERT_FALSE(answer.ok());
  EXPECT_EQ(answer.status.code, PinCountingStore::kPinRefused) << "the pin's code, handed back";
  EXPECT_NE(answer.status.detail.find("still charged"), std::string::npos) << answer.status.detail;
  ASSERT_TRUE(answer.value.id.valid()) << "the preview it could not forget";
  store.refuseForgetOfAllocated = false;
  CleanUpHandedBack(answer.value);
}

// A sphere is larger than the heap on a phone (ADR 0023), so its preview borrows one frame at a time
// and puts each back where it found it: eight spilled frames in a store with room for four of them
// beside the answer.
TEST(NearestCentrePreview, IsMadeOfASphereLargerThanTheHeapAndLeavesItWhereItWas) {
  constexpr int64_t kFrameBytes = int64_t{kWidth} * kHeight * 4;
  constexpr int64_t kPreviewBytes = 64 * 32 * 4;
  FakeSpillSink sink;
  MemoryFrameStoreAccess store{4 * kFrameBytes + kPreviewBytes, &sink};
  NearestCentreCompositionEngine engine{store};
  GlobalSolution solution;
  solution.intrinsics = LensFromFieldOfView(60.0, 46.8, kWidth, kHeight);
  std::vector<FrameRef> frames;
  for (int k = 0; k < 8; ++k) {
    Result<FrameRef> frame = store.Allocate(kWidth, kHeight, PixelFormat::RGBA8);
    ASSERT_TRUE(frame.ok()) << frame.status.detail;
    Result<std::span<uint8_t>> bytes = store.Pin(frame.value);
    ASSERT_TRUE(bytes.ok());
    std::fill(bytes.value.begin(), bytes.value.end(), static_cast<uint8_t>(10 + k));
    ASSERT_TRUE(store.Release(frame.value).ok());
    ASSERT_TRUE(store.Demote(frame.value, Residency::Spilled).ok());
    frames.push_back(frame.value);
    solution.frames.push_back(frame.value.id);
    solution.rotations.push_back(FromAzimuthElevation(45.0 * k, 0.0));
  }

  Result<FrameRef> preview = engine.RenderPreview(solution, frames, {}, 64);
  ASSERT_TRUE(preview.ok()) << preview.status.detail;
  for (const FrameRef& frame : frames) {
    EXPECT_EQ(store.ResidencyOf(frame).value, Residency::Spilled) << "put back where it was found";
  }
  Result<std::span<uint8_t>> drawn = store.Pin(preview.value);
  ASSERT_TRUE(drawn.ok());
  std::set<uint8_t> colours;
  for (int32_t x = 0; x < 64; ++x) {
    const uint8_t* pixel = drawn.value.data() + 16 * preview.value.stride + x * 4;
    EXPECT_EQ(pixel[3], 255) << "the whole equator is seen, column " << x;
    colours.insert(pixel[0]);
  }
  EXPECT_EQ(colours.size(), 8U) << "every frame coloured some of it";
  EXPECT_TRUE(store.Release(preview.value).ok());
  EXPECT_TRUE(store.Forget(preview.value).ok());
}

// The preview is written through the stride the store gives it, which may be wider than a row.
// Pixel for pixel the preview a tightly packed store gives, from a frame whose every row differs,
// so a row written at the wrong offset cannot land on one of the same colour.
TEST_F(NearestCentreComposition, TheAnswerIsWrittenThroughItsOwnStride) {
  Look(Quat{}, [](int32_t x, int32_t y) {
    return Rgb{static_cast<uint8_t>(x * 3), static_cast<uint8_t>(y * 4), 7};
  });
  const Preview packed = Render(256);
  store.padAllocatedPixels = 16;
  const Preview padded = Render(256);
  ASSERT_EQ(padded.width, packed.width);
  EXPECT_TRUE(padded.bytes == packed.bytes);
  // Every row it writes is held even where the span stops before the last row's padding.
  store.trimTailOfAllocated = 16 * 4;
  const Preview trimmed = Render(256);
  ASSERT_EQ(trimmed.width, packed.width);
  EXPECT_TRUE(trimmed.bytes == packed.bytes);
  // And one byte fewer is a last row not held. A trim shortens only the span the store answers,
  // not its allocation, so an overrun past it is invisible: the bound's refusal is what to assert.
  store.trimTailOfAllocated = 16 * 4 + 1;
  EXPECT_EQ(Refused(256), StatusCode::Internal) << "one byte short of the last row";
}

// A preview of one row or two, where the bound's branches for few rows are the only guards.
TEST_F(NearestCentreComposition, AnAnswerOfOneOrTwoRowsIsHeldToItsBytes) {
  Look(Quat{}, Rgb{1, 2, 3});
  store.trimTailOfAllocated = 4;
  EXPECT_EQ(Refused(2), StatusCode::Internal) << "a one-row answer shorter than its row";
  store.trimTailOfAllocated = 0;
  store.describeAllocated = [](FrameRef& f) { f.stride += 4; };
  EXPECT_EQ(Refused(4), StatusCode::Internal) << "a two-row answer whose stride runs past";
}

// And a frame is read through its own stride: a handle naming the left half of a wider allocation
// is read as that half, never as rows run together.
TEST_F(NearestCentreComposition, AFrameIsReadThroughItsOwnStride) {
  FrameRef wide = Paint(PixelFormat::RGBA8, [](int32_t x, int32_t y) {
    return x < kWidth ? Rgb{static_cast<uint8_t>(x * 3), static_cast<uint8_t>(y * 4), 7}
                      : Rgb{250, 250, 250};
  }, kWidth * 2);
  wide.width = kWidth;
  frames.push_back(wide);
  solution.frames.push_back(wide.id);
  solution.rotations.push_back(Quat{});
  // And the span ends where the last row ends: its padding is never read, so it need not be held.
  store.trimTailOfHandedIn = kWidth * 4;
  const Preview preview = Render(512);
  // One byte fewer and the last row is not held: refused, before anything is read past it.
  store.trimTailOfHandedIn = kWidth * 4 + 1;
  EXPECT_EQ(Refused(64), StatusCode::InvalidArgument) << "one byte short of the last row";
  store.trimTailOfHandedIn = kWidth * 4;
  int32_t checked = 0;
  for (int32_t y = 0; y < preview.height; ++y) {
    for (int32_t x = 0; x < preview.width; ++x) {
      const std::optional<Vec3> world = EquirectDirection(Pixel{x + 0.5, y + 0.5}, 512, 256);
      const ProjectedPixel at = Project(lens, *world);
      if (!at.valid || at.pixel.x < 1.0 || at.pixel.x > kWidth - 1.0 || at.pixel.y < 1.0 ||
          at.pixel.y > kHeight - 1.0) {
        continue;
      }
      const std::array<uint8_t, 4> got = preview.At(x, y);
      EXPECT_NEAR(got[0], (at.pixel.x - 0.5) * 3.0, 0.51) << x << " " << y;
      EXPECT_NEAR(got[1], (at.pixel.y - 0.5) * 4.0, 0.51) << x << " " << y;
      ++checked;
    }
  }
  EXPECT_GT(checked, 100);
}

// A stride shorter than a row is knowable from the handle, so it is refused before the frame is
// pinned: a spilled frame is not faulted in only to be turned away.
TEST_F(NearestCentreComposition, AStrideShorterThanARowIsRefusedUnread) {
  Look(Quat{}, Rgb{1, 2, 3});
  frames[0].stride = kWidth * 4 - 4;
  EXPECT_TRUE(RefusedSaying("shorter than its row"));
  EXPECT_EQ(store.PinnedEver(frames[0]), 0);
}

// A frame that colours none of the preview is never read: here a second frame looking exactly as
// the first does, which loses every tie.
TEST_F(NearestCentreComposition, AFrameThatColoursNothingIsNeverRead) {
  Look(Quat{}, Rgb{1, 2, 3});
  Look(Quat{}, Rgb{4, 5, 6});
  // Not even asked its tier: a frame the store no longer holds, where it colours nothing, is no
  // refusal.
  ASSERT_TRUE(real.Forget(frames[1]).ok());
  EXPECT_EQ(Render(64).At(32, 16), (std::array<uint8_t, 4>{1, 2, 3, 255}));
  EXPECT_EQ(store.PinnedEver(frames[0]), 1);
  EXPECT_EQ(store.PinnedEver(frames[1]), 0);
}

// A refusal whose preview the store will then not release says that too.
TEST_F(NearestCentreComposition, AnAnswerRefusedWhoseReleaseIsDeclinedSaysItIsStillCharged) {
  Look(Quat{}, Rgb{1, 2, 3});
  store.describeAllocated = [](FrameRef& f) { f.height += 1; };
  store.refuseRelease = PinCountingStore::Whose::Allocated;
  const Result<FrameRef> answer = engine.RenderPreview(solution, frames, {}, 64);
  const Status& refused = answer.status;
  EXPECT_EQ(refused.code, StatusCode::Internal);
  EXPECT_NE(refused.detail.find("would not release the preview"), std::string::npos)
      << refused.detail;
  EXPECT_EQ(refused.detail.find("would not forget"), std::string::npos)
      << "a preview still pinned is not asked to be forgotten: " << refused.detail;
  EXPECT_EQ(store.outstanding(), 1) << "the preview's own pin, which is what it says";
  ASSERT_TRUE(answer.value.id.valid()) << "and it is handed back to be released";
  CleanUpHandedBack(answer.value);
}

// A preview released but not forgotten is handed back to be forgotten, and says which it was.
TEST_F(NearestCentreComposition, AnAnswerRefusedWhoseForgetIsDeclinedIsHandedBack) {
  Look(Quat{}, Rgb{1, 2, 3});
  store.describeAllocated = [](FrameRef& f) { f.width -= 2; };
  store.refuseForgetOfAllocated = true;
  const Result<FrameRef> answer = engine.RenderPreview(solution, frames, {}, 64);
  EXPECT_EQ(answer.status.code, StatusCode::Internal);
  EXPECT_NE(answer.status.detail.find("would not forget the preview"), std::string::npos)
      << answer.status.detail;
  EXPECT_EQ(store.outstanding(), 0) << "it was released";
  ASSERT_TRUE(answer.value.id.valid());
  store.refuseForgetOfAllocated = false;
  CleanUpHandedBack(answer.value);
}

// A frame index past 255 needs both of its bytes: frame 256 of 257, the only one looking forward,
// colours the centre, where a lost high byte would name frame 0, which faces away.
TEST_F(NearestCentreComposition, AFrameIndexPastOneByteIsReadWhole) {
  const FrameRef away = Paint(PixelFormat::RGBA8, [](int32_t, int32_t) { return Rgb{1, 1, 1}; });
  for (int k = 0; k < 256; ++k) {
    frames.push_back(away);
    solution.frames.push_back(away.id);
    solution.rotations.push_back(FromAzimuthElevation(180.0, 0.0));
  }
  Look(Quat{}, Rgb{90, 80, 70});
  EXPECT_EQ(Render(64).At(32, 16), (std::array<uint8_t, 4>{90, 80, 70, 255}));
}

// A pin the store refuses takes nothing from a caller who already held one.
TEST_F(NearestCentreComposition, ARefusedPinLeavesTheCallersPinAlone) {
  Look(Quat{}, Rgb{1, 2, 3});
  ASSERT_TRUE(real.Pin(frames[0]).ok());
  store.refusePinOfHandedIn = true;
  EXPECT_EQ(Refused(), PinCountingStore::kPinRefused) << "the store's own refusal";
  EXPECT_TRUE(real.Release(frames[0]).ok()) << "the caller's pin is still there";
  EXPECT_FALSE(real.Release(frames[0]).ok()) << "and only that one";
}

// A frame its caller already holds pinned is read and left pinned, once: the preview's own pin
// comes and goes, and it does not try to demote a frame somebody else is holding.
TEST_F(NearestCentreComposition, AFrameItsCallerHoldsPinnedIsReadAndLeftPinned) {
  Look(Quat{}, Rgb{1, 2, 3});
  ASSERT_TRUE(real.Pin(frames[0]).ok());
  EXPECT_EQ(Render(64).At(32, 16)[3], 255) << "the preview is made";
  EXPECT_EQ(real.ResidencyOf(frames[0]).value, Residency::HeapPinned);
  EXPECT_TRUE(real.Release(frames[0]).ok()) << "the caller's pin is still there to release";
  EXPECT_FALSE(real.Release(frames[0]).ok()) << "and it was the only one left";
}

// A frame whose tier the store cannot say is not read: reading it would fault it in with nothing to
// say where to put it back.
TEST_F(NearestCentreComposition, AFrameWhoseTierTheStoreCannotSayIsNotRead) {
  Look(Quat{}, Rgb{1, 2, 3});
  const int64_t before = real.Budget().value.heapUsedBytes;
  store.refuseResidency = true;
  EXPECT_EQ(Refused(), PinCountingStore::kTierUnknown) << "the store's own code";
  EXPECT_EQ(real.Budget().value.heapUsedBytes, before) << "the preview is given back";
  EXPECT_EQ(store.PinnedEver(frames[0]), 0) << "and the frame was never read";
}

// While a pixel waits for its frame, its colour bytes hold that frame's index; once painted they
// are a colour. A frame painted in a colour that spells another frame's index keeps its pixels.
TEST_F(NearestCentreComposition, APaintedColourIsNotTakenForAFrameIndex) {
  Look(FromAzimuthElevation(20.0, 0.0), Rgb{1, 0, 200});  // red 1, green 0: frame 1's index
  Look(FromAzimuthElevation(-20.0, 0.0), Rgb{0, 0, 90});
  const Preview preview = Render(360);
  EXPECT_EQ(preview.At(170, 90), (std::array<uint8_t, 4>{1, 0, 200, 255}))
      << "where both frames see, the squarer one's colour stays";
  EXPECT_EQ(preview.At(190, 90), (std::array<uint8_t, 4>{0, 0, 90, 255}));
}

// One spilled frame looking forward, in a store with a sink, for the cases that are about where a
// frame is left afterwards.
struct SpilledForward {
  FakeSpillSink sink;
  MemoryFrameStoreAccess store;
  NearestCentreCompositionEngine engine{store};
  GlobalSolution solution;
  std::vector<FrameRef> frames;
  // `adopted`: spilled by an earlier session and adopted from its document, which carries no
  // content hash, so the store cannot vouch for the sink's copy and putting it back writes.
  explicit SpilledForward(bool spill = true, int64_t ceiling = int64_t{1} << 24,
                          bool adopted = false)
      : store(ceiling, &sink) {
    solution.intrinsics = LensFromFieldOfView(60.0, 46.8, kWidth, kHeight);
    Result<FrameRef> frame;
    if (adopted) {
      MemoryFrameStoreAccess earlier{ceiling, &sink};
      frame = earlier.Allocate(kWidth, kHeight, PixelFormat::RGBA8);
      EXPECT_TRUE(frame.ok());
      EXPECT_TRUE(earlier.Demote(frame.value, Residency::Spilled).ok());
      EXPECT_TRUE(store.Adopt(frame.value).ok());
    } else {
      frame = store.Allocate(kWidth, kHeight, PixelFormat::RGBA8);
      EXPECT_TRUE(frame.ok());
      if (spill) {
        EXPECT_TRUE(store.Demote(frame.value, Residency::Spilled).ok());
      }
    }
    frames.push_back(frame.value);
    solution.frames.push_back(frame.value.id);
    solution.rotations.push_back(Quat{});
  }
  int64_t HeapUsed() { return store.Budget().value.heapUsedBytes; }
};

// A refusal after the frame was pinned still puts it back: the heap it was spilled out of is not
// taken back by a preview that was never made.
TEST(NearestCentrePreview, AFrameRefusedAfterItWasPinnedIsStillPutBackWhereItWas) {
  SpilledForward spilled;
  spilled.frames[0].stride = kWidth * 4 * 2;
  Result<FrameRef> answer = spilled.engine.RenderPreview(spilled.solution, spilled.frames, {}, 64);
  EXPECT_EQ(answer.status.code, StatusCode::InvalidArgument);
  EXPECT_EQ(spilled.store.ResidencyOf(spilled.frames[0]).value, Residency::Spilled);
  EXPECT_EQ(spilled.HeapUsed(), 0) << "neither the frame nor the answer is left in the heap";
}

// A refusal that hands the preview back keeps its own code: here the handle's, refused after the
// preview was pinned, whose release the store then declines.
TEST_F(NearestCentreComposition, ARefusalHandingThePreviewBackKeepsItsOwnCode) {
  Look(Quat{}, Rgb{1, 2, 3});
  frames[0].stride = kWidth * 4 * 2;
  store.refuseRelease = PinCountingStore::Whose::Allocated;
  const Result<FrameRef> answer = engine.RenderPreview(solution, frames, {}, 64);
  EXPECT_EQ(answer.status.code, StatusCode::InvalidArgument) << answer.status.detail;
  ASSERT_TRUE(answer.value.id.valid()) << "the preview it could not release";
  CleanUpHandedBack(answer.value);
}

// A frame the store will not put back is a promise the preview could not keep, so it is a refusal
// that says so rather than an answer with a frame quietly left in the heap. Put back unchanged, a
// frame the store spilled itself needs no write; one adopted without a content hash does.
TEST(NearestCentrePreview, AFrameTheStoreWillNotPutBackIsARefusal) {
  SpilledForward spilled(true, int64_t{1} << 24, /*adopted=*/true);
  spilled.sink.FailWrites(true);
  Result<FrameRef> answer = spilled.engine.RenderPreview(spilled.solution, spilled.frames, {}, 64);
  ASSERT_FALSE(answer.ok());
  EXPECT_EQ(answer.status.code, StatusCode::FrameStoreExhausted) << "the store's own code";
  EXPECT_NE(answer.status.detail.find("tier it was found in"), std::string::npos)
      << answer.status.detail;
  EXPECT_EQ(spilled.HeapUsed(), kWidth * kHeight * 4) << "the answer is given back; the frame "
                                                           "is where the store left it";
  // The contract's recovery: a frame whose tier changed is out of place — this one released, so
  // not pinned — and goes back in the tier it had.
  const Result<Residency> now = spilled.store.ResidencyOf(spilled.frames[0]);
  ASSERT_TRUE(now.ok());
  EXPECT_NE(now.value, Residency::Spilled) << "its tier changed";
  EXPECT_NE(now.value, Residency::HeapPinned) << "so there is no pin to release";
  spilled.sink.FailWrites(false);
  EXPECT_TRUE(spilled.store.Demote(spilled.frames[0], Residency::Spilled).ok());
  EXPECT_EQ(spilled.HeapUsed(), 0) << "and it is back where it was found";
}

// A preview only reads, so the frames it borrows go back to the sink without being written again.
TEST(NearestCentrePreview, ReadingASpilledFrameWritesNothingBack) {
  SpilledForward spilled;
  const int written = spilled.sink.Writes();
  Result<FrameRef> answer = spilled.engine.RenderPreview(spilled.solution, spilled.frames, {}, 64);
  ASSERT_TRUE(answer.ok()) << answer.status.detail;
  EXPECT_EQ(spilled.sink.Writes(), written);
  EXPECT_EQ(spilled.store.ResidencyOf(spilled.frames[0]).value, Residency::Spilled);
  EXPECT_TRUE(spilled.store.Forget(answer.value).ok());
}

// A frame that cannot be pinned is refused with the store's own code: here a spilled one with no room
// left to fault it into beside the preview.
TEST(NearestCentrePreview, AFrameThatCannotBePinnedIsTheStoresOwnRefusal) {
  SpilledForward full(true, int64_t{kWidth} * kHeight * 4 + 64 * 32 * 4 - 1);
  Result<FrameRef> answer = full.engine.RenderPreview(full.solution, full.frames, {}, 64);
  EXPECT_EQ(answer.status.code, StatusCode::FrameStoreExhausted) << answer.status.detail;
  EXPECT_EQ(full.store.ResidencyOf(full.frames[0]).value, Residency::Spilled);
  EXPECT_EQ(full.HeapUsed(), 0) << "the preview is given back";
}

// And one found in the heap is left there, in a store that could have spilled it.
TEST(NearestCentrePreview, AFrameFoundInTheHeapIsLeftInTheHeap) {
  SpilledForward resident(false);
  Result<FrameRef> answer = resident.engine.RenderPreview(resident.solution, resident.frames, {}, 64);
  ASSERT_TRUE(answer.ok()) << answer.status.detail;
  EXPECT_EQ(resident.store.ResidencyOf(resident.frames[0]).value, Residency::HeapEncoded);
  EXPECT_TRUE(resident.store.Forget(answer.value).ok());
}

// A handle refused after its pin, whose release the store then declines, says so: the frame is
// still pinned, and its caller cannot forget it until that pin goes.
TEST_F(NearestCentreComposition, AFrameRefusedAfterItWasPinnedWhoseReleaseIsDeclinedSaysSo) {
  Look(Quat{}, Rgb{1, 2, 3});
  frames[0].stride = kWidth * 4 * 2;
  store.refuseRelease = PinCountingStore::Whose::HandedIn;
  const Status refused = engine.RenderPreview(solution, frames, {}, 64).status;
  EXPECT_EQ(refused.code, StatusCode::InvalidArgument) << "the refusal is still the handle's";
  EXPECT_NE(refused.detail.find("declined to release"), std::string::npos) << refused.detail;
  EXPECT_EQ(store.outstanding(), 1) << "still pinned, as it says";
  EXPECT_TRUE(real.Release(frames[0]).ok());
}

// A frame left pinned is named, by its place in the solution and its id: its caller may hold pins
// of its own on the others, and `ResidencyOf` cannot say which pin is whose.
// The second of four, so neither the first nor the last borrowed nor its own place counted from the
// end; and its buffer relabelled, since the memory store issues the id and the buffer from one
// counter and would otherwise let a name spelt from either pass for the other.
TEST_F(NearestCentreComposition, AFrameLeftPinnedIsNamed) {
  Look(Quat{}, Rgb{1, 2, 3});
  Look(FromAzimuthElevation(90.0, 0.0), Rgb{4, 5, 6});
  Look(FromAzimuthElevation(180.0, 0.0), Rgb{7, 8, 9});
  Look(FromAzimuthElevation(270.0, 0.0), Rgb{10, 11, 12});
  frames[1].buffer = BufferId{frames[1].buffer.value + 1000};
  store.refuseRelease = PinCountingStore::Whose::HandedIn;
  store.refuseReleaseOnlyOf = frames[1].id;
  const Status refused = engine.RenderPreview(solution, frames, {}, 64).status;
  const std::string named =
      "frame 1 of the solution (id " + std::to_string(frames[1].id.value) + ")";
  EXPECT_NE(refused.detail.find(named), std::string::npos) << refused.detail;
  EXPECT_EQ(store.outstanding(), 1);
  EXPECT_TRUE(store.Release(frames[1]).ok()) << "the frame it names is the one still pinned";
}

// A handle naming an id the store has not issued yet is not a frame — and `Allocate` is about to
// issue it, to the preview, so it would be read as the preview itself. A handle from a session the
// store never adopted can carry one, since a new store counts from 1 again.
TEST_F(NearestCentreComposition, AHandleNamingThePreviewIsRefused) {
  Look(Quat{}, Rgb{1, 2, 3});
  Result<FrameRef> probe = real.Allocate(2, 2, PixelFormat::RGBA8);
  ASSERT_TRUE(probe.ok());
  ASSERT_TRUE(real.Forget(probe.value).ok());
  // Only the id: the store names frames by it, so a guard asking the buffer would miss this.
  frames[0].id = FrameId{probe.value.id.value + 1};
  solution.frames[0] = frames[0].id;
  const int64_t before = real.Budget().value.heapUsedBytes;
  EXPECT_TRUE(RefusedSaying("the preview itself"));
  EXPECT_EQ(real.Budget().value.heapUsedBytes, before) << "the preview is forgotten";
}

// A frame its caller already held pinned was never out of its tier, so a declined release asks only
// for this call's pin to be released: there is nothing to put back, and a demotion to `HeapPinned`
// is one the store refuses.
TEST_F(NearestCentreComposition, AFrameFoundPinnedWhoseReleaseIsDeclinedIsNotToBePutBack) {
  Look(Quat{}, Rgb{1, 2, 3});
  ASSERT_TRUE(real.Pin(frames[0]).ok());
  store.refuseRelease = PinCountingStore::Whose::HandedIn;
  const Status refused = engine.RenderPreview(solution, frames, {}, 64).status;
  EXPECT_NE(refused.detail.find("declined to release"), std::string::npos) << refused.detail;
  EXPECT_EQ(refused.detail.find("put back"), std::string::npos) << refused.detail;
  EXPECT_TRUE(real.Release(frames[0]).ok()) << "this call's pin";
  EXPECT_EQ(real.ResidencyOf(frames[0]).value, Residency::HeapPinned) << "the caller's, still";
  EXPECT_TRUE(real.Release(frames[0]).ok()) << "and the caller's own";
}

// A declined release is not followed by a demotion nobody hears about: the frame is left exactly as
// reported — pinned, in the heap — and not half put back.
TEST(NearestCentrePreview, AFrameWhoseReleaseIsDeclinedIsLeftAsReported) {
  for (const auto& [tier, name] : {std::pair{Residency::Spilled, "Spilled"},
                                   std::pair{Residency::HeapEncoded, "HeapEncoded"}}) {
    FakeSpillSink sink;
    MemoryFrameStoreAccess real{int64_t{1} << 24, &sink};
    PinCountingStore store{real};
    NearestCentreCompositionEngine engine{store};
    GlobalSolution solution;
    solution.intrinsics = LensFromFieldOfView(60.0, 46.8, kWidth, kHeight);
    Result<FrameRef> frame = real.Allocate(kWidth, kHeight, PixelFormat::RGBA8);
    ASSERT_TRUE(frame.ok());
    ASSERT_TRUE(real.Demote(frame.value, tier).ok());
    const int64_t found = real.Budget().value.heapUsedBytes;
    const std::vector<FrameRef> frames = {frame.value};
    solution.frames.push_back(frame.value.id);
    solution.rotations.push_back(Quat{});
    store.refuseRelease = PinCountingStore::Whose::HandedIn;
    sink.FailWrites(true);
    const Status refused = engine.RenderPreview(solution, frames, {}, 64).status;
    EXPECT_NE(refused.detail.find("still pinned"), std::string::npos) << name << ": " << refused.detail;
    EXPECT_NE(refused.detail.find(std::string("put back in ") + name), std::string::npos)
        << "the tier the caller has to restore, which only the call knew: " << refused.detail;
    EXPECT_EQ(real.ResidencyOf(frame.value).value, Residency::HeapPinned) << name;
    // Doing what it says restores what the call found.
    EXPECT_TRUE(real.Release(frame.value).ok()) << name << ": the pin the refusal names";
    sink.FailWrites(false);
    EXPECT_TRUE(real.Demote(frame.value, tier).ok()) << name;
    EXPECT_EQ(real.ResidencyOf(frame.value).value, tier) << name;
    EXPECT_EQ(real.Budget().value.heapUsedBytes, found)
        << name << ": the preview was given back, the frame put back";
  }
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

#include "engines/composition_engine/nearest_centre_composition_engine.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "utilities/camera_model.h"
#include "utilities/equirect.h"
#include "utilities/quaternion.h"

namespace sphanorama {
namespace {

constexpr const char* kComponent = "NearestCentreCompositionEngine";
constexpr int32_t kChannels = 4;
// Each preview pixel records the frame that colours it in two of its own bytes, and this value
// means none, so the last frame a solution may name is the one before it.
constexpr uint16_t kNoFrame = std::numeric_limits<uint16_t>::max();

// One frame of the caller's, pinned while its pixels are painted and then given back: released, and
// demoted to the tier it was found in. Frames are borrowed one at a time so that the preview of a
// sphere larger than the heap can still be made, and the demotion is what keeps it true — a pin
// faults a spilled frame in (the rule `CandidatePreview` follows, and for the same reason). A frame
// found pinned was pinned by somebody whose pin outlives this call, so it is where it was once the
// release is done, and demoting it would be refused.
//
// Every path that pinned calls `GiveBack` and reports what it answers; the destructor only retries
// a release the store declined, on a path already reporting that it did.
class Borrowed {
 public:
  Borrowed(IFrameStoreAccess& store, const FrameRef& frame)
      : store_(store), frame_(frame), before_(store.ResidencyOf(frame)) {}
  Borrowed(const Borrowed&) = delete;
  Borrowed& operator=(const Borrowed&) = delete;
  ~Borrowed() {
    if (pinned_) (void)GiveBack();
  }
  const Result<Residency>& before() const { return before_; }
  Result<std::span<uint8_t>> Pin() {
    Result<std::span<uint8_t>> pinned = store_.Pin(frame_);
    pinned_ = pinned.ok();
    return pinned;
  }
  Status GiveBack() {
    if (Status released = store_.Release(frame_); !released.ok()) {
      return Fail(released.code, kComponent,
                  "the store declined to release a frame handed in: " + released.detail);
    }
    pinned_ = false;
    if (before_.value == Residency::HeapPinned) return Status::Ok();
    if (Status demoted = store_.Demote(frame_, before_.value); !demoted.ok()) {
      return Fail(demoted.code, kComponent,
                  "a frame handed in could not be put back in the tier it was found in: " +
                      demoted.detail);
    }
    return Status::Ok();
  }

 private:
  IFrameStoreAccess& store_;
  FrameRef frame_;
  Result<Residency> before_;
  bool pinned_ = false;
};

// Bilinear, between pixel centres, clamped at the frame's edge: a direction the lens puts between
// the edge and the outermost centre takes the outermost pixel rather than a blend with nothing.
std::array<double, 3> Sample(std::span<const uint8_t> bytes, const FrameRef& frame, const Pixel& at) {
  const double x = std::clamp(at.x - 0.5, 0.0, static_cast<double>(frame.width - 1));
  const double y = std::clamp(at.y - 0.5, 0.0, static_cast<double>(frame.height - 1));
  const int32_t x0 = static_cast<int32_t>(x);
  const int32_t y0 = static_cast<int32_t>(y);
  const int32_t x1 = std::min(x0 + 1, frame.width - 1);
  const int32_t y1 = std::min(y0 + 1, frame.height - 1);
  const double fx = x - x0;
  const double fy = y - y0;
  const auto channel = [&](int32_t px, int32_t py, int32_t c) {
    return static_cast<double>(bytes[static_cast<size_t>(py) * static_cast<size_t>(frame.stride) +
                                     static_cast<size_t>(px) * kChannels + static_cast<size_t>(c)]);
  };
  std::array<double, 3> rgb{};
  for (int32_t c = 0; c < 3; ++c) {
    const double top = channel(x0, y0, c) * (1.0 - fx) + channel(x1, y0, c) * fx;
    const double bottom = channel(x0, y1, c) * (1.0 - fx) + channel(x1, y1, c) * fx;
    rgb[static_cast<size_t>(c)] = top * (1.0 - fy) + bottom * fy;
  }
  return rgb;
}

}  // namespace

Result<GainMap> NearestCentreCompositionEngine::CompensateExposure(const GlobalSolution&,
                                                                   std::span<const FrameRef>) {
  return Err<GainMap>(StatusCode::Unsupported, kComponent, "exposure compensation is not built");
}

Result<GhostReport> NearestCentreCompositionEngine::DetectGhosts(const GlobalSolution&,
                                                                 std::span<const Candidate>) {
  return Err<GhostReport>(StatusCode::Unsupported, kComponent, "ghost detection is not built");
}

Result<SeamMap> NearestCentreCompositionEngine::FindSeams(const GlobalSolution&,
                                                          std::span<const FrameRef>,
                                                          const GainMap&, const GhostReport&,
                                                          const BuildSpec&) {
  return Err<SeamMap>(StatusCode::Unsupported, kComponent, "seam finding is not built");
}

Result<FrameRef> NearestCentreCompositionEngine::BlendTile(const GlobalSolution&,
                                                           std::span<const FrameRef>,
                                                           const GainMap&, const SeamMap&,
                                                           const BuildSpec&, int32_t, int32_t) {
  return Err<FrameRef>(StatusCode::Unsupported, kComponent, "blending is not built");
}

Result<FrameRef> NearestCentreCompositionEngine::RenderPreview(const GlobalSolution& solution,
                                                               std::span<const FrameRef> frames,
                                                               const GainMap& gains,
                                                               int32_t maxWidth) {
  if (maxWidth < 2) {
    return Err<FrameRef>(StatusCode::InvalidArgument, kComponent,
                         "a panorama narrower than two pixels has no half to be high");
  }
  const size_t count = solution.frames.size();
  if (count > kNoFrame) {
    return Err<FrameRef>(StatusCode::InvalidArgument, kComponent,
                         "more frames than a preview can tell apart");
  }
  if (solution.rotations.size() != count || frames.size() != count) {
    return Err<FrameRef>(StatusCode::InvalidArgument, kComponent,
                         "the frames and rotations are not the solution's, one each");
  }
  const Intrinsics& lens = solution.intrinsics;
  if (count > 0 && !IsUsableLens(lens)) {
    return Err<FrameRef>(StatusCode::InvalidArgument, kComponent, "the lens cannot project");
  }
  for (size_t i = 0; i < count; ++i) {
    if (!(frames[i].id == solution.frames[i])) {
      return Err<FrameRef>(StatusCode::InvalidArgument, kComponent,
                           "the frames are not the solution's, in its order");
    }
    if (!IsUsableRotation(solution.rotations[i])) {
      return Err<FrameRef>(StatusCode::InvalidArgument, kComponent, "a rotation is not one");
    }
    if (frames[i].format != PixelFormat::RGBA8) {
      return Err<FrameRef>(StatusCode::Unsupported, kComponent, "a frame is not RGBA8");
    }
    if (frames[i].width != lens.width || frames[i].height != lens.height) {
      return Err<FrameRef>(StatusCode::InvalidArgument, kComponent,
                           "a frame is another size than the lens");
    }
  }
  std::vector<double> gain(count, 1.0);
  if (!gains.perFrameGain.empty() || !gains.frames.empty()) {
    if (gains.perFrameGain.size() != count || gains.frames.size() != count) {
      return Err<FrameRef>(StatusCode::InvalidArgument, kComponent,
                           "the gains do not name the solution's frames");
    }
    for (size_t i = 0; i < count; ++i) {
      if (!(gains.frames[i] == solution.frames[i])) {
        return Err<FrameRef>(StatusCode::InvalidArgument, kComponent,
                             "the gains do not name the solution's frames in its order");
      }
      const double g = gains.perFrameGain[i];
      if (!std::isfinite(g) || g <= 0.0) {
        return Err<FrameRef>(StatusCode::InvalidArgument, kComponent,
                             "a gain is not a figure above zero");
      }
      gain[i] = g;
    }
  }

  const int32_t width = maxWidth & ~1;
  const int32_t height = width / 2;
  Result<FrameRef> answer = frames_.Allocate(width, height, PixelFormat::RGBA8);
  if (!answer.ok()) return answer.status;

  // Gives the answer back on a refusal, and says so when the store will not take it, since its
  // bytes then stay charged with nothing else reporting them.
  auto abandon = [&](StatusCode code, std::string detail, bool pinned) {
    if (pinned && !frames_.Release(answer.value).ok()) {
      detail += "; and the store would not release the preview, so its bytes are still charged";
    } else if (!frames_.Forget(answer.value).ok()) {
      detail += "; and the store would not forget the preview, so its bytes are still charged";
    }
    return Err<FrameRef>(code, kComponent, std::move(detail));
  };

  Result<std::span<uint8_t>> out = frames_.Pin(answer.value);
  if (!out.ok()) return abandon(out.status.code, out.status.detail, false);
  // The rows are written through the handle, and `Allocate` promises nothing about stride, so the
  // handle is held to the bytes before anything is written through it.
  const FrameRef& drawn = answer.value;
  if (drawn.width != width || drawn.height != height ||
      drawn.stride < static_cast<int64_t>(width) * kChannels ||
      drawn.stride > static_cast<int64_t>(out.value.size()) / height) {
    return abandon(StatusCode::Internal,
                   "the store described the preview's frame as another shape than was asked for",
                   true);
  }
  std::memset(out.value.data(), 0, out.value.size());

  // Which frame colours each pixel is a question of geometry alone, so it is answered for the
  // whole preview before any frame is pinned.
  std::vector<Vec3> axes(count);
  std::vector<Quat> toCamera(count);
  for (size_t i = 0; i < count; ++i) {
    axes[i] = Rotate(solution.rotations[i], Vec3{0.0, 0.0, -1.0});
    toCamera[i] = Conjugate(solution.rotations[i]);
  }
  auto directionAt = [&](int32_t x, int32_t y) {
    return EquirectDirection(Pixel{x + 0.5, y + 0.5}, width, height);
  };
  auto landing = [&](size_t i, const Vec3& direction) -> std::optional<Pixel> {
    const ProjectedPixel projected = Project(lens, Rotate(toCamera[i], direction));
    if (!projected.valid || projected.pixel.x < 0.0 || projected.pixel.y < 0.0 ||
        projected.pixel.x > lens.width || projected.pixel.y > lens.height) {
      return std::nullopt;
    }
    return projected.pixel;
  };
  // The index is kept in the answer's own bytes until the pixel is painted — red and green hold
  // the frame, alpha 0 says not yet painted — so the preview costs one frame and the answer and
  // nothing the store does not charge for.
  auto pixelAt = [&](int32_t x, int32_t y) {
    return out.value.data() + static_cast<size_t>(y) * static_cast<size_t>(drawn.stride) +
           static_cast<size_t>(x) * kChannels;
  };
  auto indexAt = [&](const uint8_t* pixel) {
    return static_cast<uint16_t>(pixel[0] | (pixel[1] << 8));
  };
  std::vector<bool> chosenAnywhere(count, false);
  for (int32_t y = 0; y < height; ++y) {
    for (int32_t x = 0; x < width; ++x) {
      uint16_t chosen = kNoFrame;
      if (const std::optional<Vec3> direction = directionAt(x, y); direction.has_value()) {
        double squarest = -std::numeric_limits<double>::infinity();
        for (size_t i = 0; i < count; ++i) {
          const double facing = Dot(axes[i], *direction);
          if (facing <= squarest || !landing(i, *direction).has_value()) continue;
          squarest = facing;
          chosen = static_cast<uint16_t>(i);
        }
      }
      if (chosen != kNoFrame) chosenAnywhere[chosen] = true;
      uint8_t* pixel = pixelAt(x, y);
      pixel[0] = static_cast<uint8_t>(chosen & 0xFF);
      pixel[1] = static_cast<uint8_t>(chosen >> 8);
    }
  }

  for (size_t i = 0; i < count; ++i) {
    if (!chosenAnywhere[i]) continue;
    const FrameRef& frame = frames[i];
    Borrowed borrowed(frames_, frame);
    if (!borrowed.before().ok()) {
      return abandon(borrowed.before().status.code,
                     "the store could not say which tier a frame handed in is in, so it cannot be "
                     "read and put back: " + borrowed.before().status.detail, true);
    }
    Result<std::span<uint8_t>> pinned = borrowed.Pin();
    if (!pinned.ok()) return abandon(pinned.status.code, pinned.status.detail, true);
    // A frame is a value the caller hands in; the pinned span is what the store really holds.
    const int64_t rowBytes = static_cast<int64_t>(frame.width) * kChannels;
    if (frame.stride < rowBytes ||
        static_cast<int64_t>(frame.stride) * frame.height > static_cast<int64_t>(pinned.value.size())) {
      std::string detail = "a frame claims more rows or a longer row than the store holds";
      if (Status given = borrowed.GiveBack(); !given.ok()) detail += "; and " + given.detail;
      return abandon(StatusCode::InvalidArgument, std::move(detail), true);
    }
    for (int32_t y = 0; y < height; ++y) {
      for (int32_t x = 0; x < width; ++x) {
        uint8_t* pixel = pixelAt(x, y);
        if (pixel[3] != 0 || indexAt(pixel) != i) continue;
        // Chosen above only where it lands, so both are answers here.
        const std::optional<Pixel> at = landing(i, *directionAt(x, y));
        const std::array<double, 3> rgb = Sample(pinned.value, frame, *at);
        for (size_t c = 0; c < 3; ++c) {
          pixel[c] = static_cast<uint8_t>(std::min(255.0, std::round(rgb[c] * gain[i])));
        }
        pixel[3] = 255;
      }
    }
    if (Status given = borrowed.GiveBack(); !given.ok()) {
      return abandon(given.code, given.detail, true);
    }
  }
  // What no frame sees is transparent black, not the index it was left holding.
  for (int32_t y = 0; y < height; ++y) {
    for (int32_t x = 0; x < width; ++x) {
      uint8_t* pixel = pixelAt(x, y);
      if (pixel[3] == 0) pixel[0] = pixel[1] = pixel[2] = 0;
    }
  }

  if (Status released = frames_.Release(answer.value); !released.ok()) {
    return Err<FrameRef>(released.code, kComponent,
                         "the preview could not be released: " + released.detail +
                             "; it is still pinned, so it cannot be forgotten and its bytes are "
                             "still charged");
  }
  return answer;
}

}  // namespace sphanorama

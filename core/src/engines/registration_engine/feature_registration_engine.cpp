#include "engines/registration_engine/feature_registration_engine.h"

#include <opencv2/core.hpp>
#include <opencv2/features2d.hpp>
#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <cstring>
#include <exception>
#include <string>
#include <vector>

#include "utilities/pixel_format.h"

namespace sphanorama {
namespace {

constexpr const char* kComponent = "FeatureRegistrationEngine";

// x and y as two float32s. The smallest thing a matcher needs, and the reason the keypoint frame
// exists separately from the descriptor frame: their row widths differ per detector.
constexpr int32_t kKeypointBytes = 8;

// The most features any detector may return for one frame.
//
// Not a tuning knob: without it two of the three detectors are unbounded. `cv::ORB::create()` caps
// itself at 500 by default, `cv::SIFT::create()` takes `nfeatures = 0` meaning retain everything,
// and `cv::AKAZE::create()` takes `max_points = -1` meaning the same. Measured on this repository's
// own test texture at 2048x2048: ORB flatlines at 500, SIFT returns 7,567 (3.87 MB of descriptors
// for one frame, 232 MB over the sixty a sphere plans) and AKAZE returns 14,656. Decimal megabytes
// throughout, as ADR 0051 counts them — an earlier draft said 221, which is the same number in MiB
// and made one sentence use both units. A capture frame is
// several times that again in pixels.
//
// It bounds the allocation, and it is also what makes the comparison the roadmap wants mean
// anything: "which detector wins is a measurement" is not a measurement if one of them is asked for
// 500 features and another for all of them. Same budget, then measure.
constexpr int kMaxFeaturesPerFrame = 500;

bool HasReadableLuma(PixelFormat format) {
  switch (format) {
    case PixelFormat::RGBA8:
    case PixelFormat::BGRA8:
    case PixelFormat::NV12:
    case PixelFormat::I420:
    case PixelFormat::Gray8:
      return true;
    default:
      return false;
  }
}

cv::Ptr<cv::Feature2D> Make(FeatureDetector detector) {
  switch (detector) {
    case FeatureDetector::Orb:
      return cv::ORB::create(kMaxFeaturesPerFrame);
    case FeatureDetector::Sift:
      return cv::SIFT::create(kMaxFeaturesPerFrame);
    case FeatureDetector::Akaze:
      // The cap is AKAZE's last parameter, so every default before it has to be restated. They are
      // OpenCV's own, copied from the declaration rather than chosen here.
      return cv::AKAZE::create(cv::AKAZE::DESCRIPTOR_MLDB, 0, 3, 0.001f, 4, 4, cv::KAZE::DIFF_PM_G2,
                               kMaxFeaturesPerFrame);
  }
  return {};
}

// The luma plane as a single-channel Mat, without copying where the layout already allows it.
cv::Mat LumaOf(const FrameRef& frame, std::span<uint8_t> bytes, size_t stride) {
  switch (frame.format) {
    case PixelFormat::Gray8:
    case PixelFormat::NV12:
    case PixelFormat::I420:
      // All three lead with a full-resolution luma plane, which is all a detector reads.
      return cv::Mat(frame.height, frame.width, CV_8UC1, bytes.data(), stride);
    case PixelFormat::RGBA8:
    case PixelFormat::BGRA8: {
      const cv::Mat colour(frame.height, frame.width, CV_8UC4, bytes.data(), stride);
      cv::Mat grey;
      cv::cvtColor(colour, grey,
                   frame.format == PixelFormat::RGBA8 ? cv::COLOR_RGBA2GRAY : cv::COLOR_BGRA2GRAY);
      return grey;
    }
    default:
      return {};
  }
}

/**
 * A frame this engine allocated, forgotten again unless the extraction commits it.
 *
 * The manual rollback this replaces was correct for the two failures it was written for and could
 * not be correct for a third: a `cv::Exception` unwinds past every `return` an error path is made
 * of. Ownership of a half-built answer is what a destructor is for, and the distinction that makes
 * it safe is unchanged — these frames are *this call's*, created here and seen by nobody, so
 * forgetting them drops nothing a caller could still want. A frame handed *in* is never forgotten.
 */
class OwnedFrame {
 public:
  OwnedFrame(IFrameStoreAccess& frames, const FrameRef& frame) : frames_(frames), frame_(frame) {}
  ~OwnedFrame() {
    if (pinned_) (void)frames_.Release(frame_);
    if (!committed_) (void)frames_.Forget(frame_);
  }
  OwnedFrame(const OwnedFrame&) = delete;
  OwnedFrame& operator=(const OwnedFrame&) = delete;

  Result<std::span<uint8_t>> Pin() {
    Result<std::span<uint8_t>> pinned = frames_.Pin(frame_);
    if (pinned.ok()) pinned_ = true;
    return pinned;
  }

  /** Keep it: the caller owns it from here, and is the one that must `Forget` it. */
  void Commit() { committed_ = true; }

  const FrameRef& ref() const { return frame_; }

 private:
  IFrameStoreAccess& frames_;
  FrameRef frame_;
  bool pinned_ = false;
  bool committed_ = false;
};

}  // namespace

Result<FeatureSet> FeatureRegistrationEngine::Extract(const FrameRef& frame) {
  if (!HasReadableLuma(frame.format)) {
    return Err<FeatureSet>(StatusCode::Unsupported, kComponent,
                           "this frame has no luma plane to read; an encoded frame needs decoding "
                           "before features can be found in it");
  }
  if (frame.width <= 0 || frame.height <= 0) {
    return Err<FeatureSet>(StatusCode::InvalidArgument, kComponent, "a frame with no pixels");
  }

  Result<std::span<uint8_t>> pinned = frames_.Pin(frame);
  if (!pinned.ok()) return pinned.status;

  // Released on every path out, failures and exceptions included. Pin promises the mapping until
  // Release, and an engine that kept one would pin a frame per extraction until the session ended.
  struct Unpin {
    IFrameStoreAccess& frames;
    const FrameRef& frame;
    ~Unpin() { (void)frames.Release(frame); }
  } unpin{frames_, frame};

  // **What the handle claims, against what the store actually handed over.** A `FrameRef` is a
  // plain value the caller passes in and `Find` keys on `id` alone, so nothing upstream makes its
  // geometry describe the allocation: the width, height and stride are the *caller's* account of a
  // frame, and `Pin` returns the entry's real span regardless. Without this, every read below
  // indexed that span with that account — a frame allocated honestly at 128x128 and then extracted
  // under a handle claiming one row more gives `heap-buffer-overflow READ of size 128` inside
  // `detectAndCompute`, which a reviewer reproduced here under AddressSanitizer.
  //
  // `SharpnessFrameQualityEngine::Measure` carries the same two checks, added after the same
  // overflow was reproduced there. This engine was written from that one — its `HasReadableLuma`,
  // its `Unpin`, its width and height line — and stopped one guard short, which is how a copied
  // function loses the part that is not about what it obviously does.
  //
  // In int64 and narrowed once: the obvious form is int32 * int32, and a width near the top of the
  // range times four bytes a pixel overflows before the comparison that would have refused it.
  // One byte per pixel for the planar formats, which is all `LumaOf` reads of them.
  const int64_t bytesPerPixel = std::max(BytesPerPixel(frame.format), 1);
  const int64_t rowBytes = static_cast<int64_t>(frame.width) * bytesPerPixel;
  const int64_t stride = frame.stride > 0 ? frame.stride : rowBytes;
  if (stride < rowBytes) {
    // Not because it reads out of bounds — it does not. Rows that overlap are not a frame anybody
    // allocated. Refusing it here rather than letting `cv::Mat` assert also keeps the answer a
    // refusal a caller can branch on, instead of an `Internal` carrying OpenCV's text.
    return Err<FeatureSet>(StatusCode::InvalidArgument, kComponent,
                           "this frame's stride is narrower than one row of it");
  }
  const int64_t held = static_cast<int64_t>(pinned.value.size());
  if (rowBytes > held) {
    return Err<FeatureSet>(StatusCode::InvalidArgument, kComponent,
                           "this frame claims more pixels than the store is holding for it");
  }
  // **Asked by division, because the multiply is the thing that overflows.** An earlier version of
  // this guard widened `width * bytesPerPixel` into int64 and then multiplied again without asking
  // the same question of the second product: with `stride <= 0` the fallback is `rowBytes`, which is
  // bounded by 2^33 rather than by `int32_t`, so `INT32_MAX` rows of it reached ~1.8e19 and wrapped
  // negative — and a negative total is smaller than any size, so the guard passed exactly the handle
  // it exists to refuse.
  //
  // Which of the two checks actually refuses that handle is worth being exact about, because the
  // test above passes either way and it would be easy to credit the wrong one. It is the row-bytes
  // check: a width big enough to overflow the product is a width whose single row already exceeds
  // anything the store is holding. That in turn bounds the fallback stride, so with both checks
  // present the product below cannot reach the top of `int64_t` from any allocation that fits in
  // memory. The division form is kept anyway — not as a second guard, but because it is how this
  // one is written so that nobody has to redo that argument when a caller or a format changes.
  const int64_t rows = static_cast<int64_t>(frame.height) - 1;
  if (rows > 0 && stride > (held - rowBytes) / rows) {
    return Err<FeatureSet>(StatusCode::InvalidArgument, kComponent,
                           "this frame claims more pixels than the store is holding for it");
  }
  // A planar frame's buffer holds chroma after the luma plane, so the rows above bound the *bytes*
  // and not the *picture*: a real I420 128x128 is 24,576 bytes, and a handle claiming 192 rows needs
  // exactly that by luma arithmetic alone. The whole-frame size is what says how much of it is
  // picture, and it is in bounds either way — so ASan never had anything to say about it.
  if (BytesPerPixel(frame.format) <= 0) {
    const int64_t whole = FrameByteSize(frame.width, frame.height, frame.format);
    if (whole <= 0 || whole > held) {
      return Err<FeatureSet>(StatusCode::InvalidArgument, kComponent,
                             "this frame claims more rows of picture than the store is holding for "
                             "it; the bytes past the luma plane are chroma");
    }
  }

  const cv::Mat luma = LumaOf(frame, pinned.value, static_cast<size_t>(stride));
  if (luma.empty()) {
    return Err<FeatureSet>(StatusCode::Unsupported, kComponent, "no luma plane in this format");
  }

  cv::Ptr<cv::Feature2D> detector = Make(detector_);
  if (!detector) {
    return Err<FeatureSet>(StatusCode::Unsupported, kComponent, "no such detector");
  }

  std::vector<cv::KeyPoint> keypoints;
  cv::Mat descriptors;
  detector->detectAndCompute(luma, cv::noArray(), keypoints, descriptors);

  // **Asking for a cap is not the same as getting one.** `KeyPointsFilter::retainBest` keeps every
  // keypoint tied with the last one at the cutoff score, so a detector asked for 500 can answer
  // with more: SIFT returned 501 on this repository's own test texture at 768 square, the first
  // time a test asserted the bound instead of assuming it. One over is harmless; the number is not
  // bounded by anything we control, which is the part that matters for an allocation sized from it.
  //
  // Truncating is safe because the detectors return their features best-first — that ordering is
  // what `retainBest` exists to produce — so the ones dropped are the ones a matcher wanted least.
  if (keypoints.size() > static_cast<size_t>(kMaxFeaturesPerFrame)) {
    keypoints.resize(static_cast<size_t>(kMaxFeaturesPerFrame));
  }

  FeatureSet features;
  features.frame = frame.id;
  features.count = static_cast<int32_t>(keypoints.size());

  // A frame with nothing in it is answered with nothing, rather than with two empty allocations
  // the caller would then have to remember to forget. `count == 0` is the whole answer.
  if (features.count == 0) return Ok(features);

  // The detector's two outputs against each other, before `count` is used to index either. The
  // loop below reads `descriptors.ptr(row)` for every row it copies, and `cv::Mat::ptr(int)` only
  // `CV_DbgAssert`s its argument — so a detector returning fewer descriptor rows than keypoints
  // would read past the Mat with no diagnostic in a release build. No detector here disagrees
  // today; this is the same assumption the code below explicitly refuses to make about the store,
  // and it costs one comparison to stop making it here too.
  if (descriptors.rows < features.count) {
    return Err<FeatureSet>(StatusCode::Internal, kComponent,
                           "the detector returned fewer descriptors than keypoints");
  }

  const int32_t descriptorBytes = static_cast<int32_t>(descriptors.cols * descriptors.elemSize());

  Result<FrameRef> descriptorAllocation =
      frames_.Allocate(descriptorBytes, features.count, PixelFormat::Gray8);
  if (!descriptorAllocation.ok()) return descriptorAllocation.status;
  OwnedFrame descriptorFrame{frames_, descriptorAllocation.value};

  Result<FrameRef> keypointAllocation =
      frames_.Allocate(kKeypointBytes, features.count, PixelFormat::Gray8);
  if (!keypointAllocation.ok()) return keypointAllocation.status;
  OwnedFrame keypointFrame{frames_, keypointAllocation.value};

  Result<std::span<uint8_t>> descriptorSpan = descriptorFrame.Pin();
  if (!descriptorSpan.ok()) return descriptorSpan.status;
  Result<std::span<uint8_t>> keypointSpan = keypointFrame.Pin();
  if (!keypointSpan.ok()) return keypointSpan.status;

  // The store returned frames of the shape asked for, and this checks rather than assumes it. A
  // `memcpy` loop sized by `count` into a span sized by whatever came back is the shape that
  // overflows silently — found by a sabotage that shrank the allocation by one row and killed the
  // test binary outright, which under a harness reading gtest's summary looks exactly like a
  // sabotage that changed nothing.
  //
  // Both dimensions, not just the total. `IFrameStoreAccess::Allocate` promises nothing about
  // stride and the shared contract suite asserts nothing about packing — tight rows are
  // `MemoryFrameStoreAccess`'s arithmetic, not the contract's. The loop below uses `descriptorBytes`
  // as its row pitch, so a store that padded rows would write every row at the wrong offset while
  // the total size still fit, which a size check alone cannot see. `FeatureSet` promises its
  // callers `stride == width`; this is where that promise is kept rather than hoped for.
  const size_t descriptorNeeded = static_cast<size_t>(features.count) * descriptorBytes;
  const size_t keypointNeeded = static_cast<size_t>(features.count) * kKeypointBytes;
  if (descriptorSpan.value.size() < descriptorNeeded ||
      keypointSpan.value.size() < keypointNeeded) {
    return Err<FeatureSet>(StatusCode::Internal, kComponent,
                           "the store returned a frame smaller than the one asked for");
  }
  if (descriptorFrame.ref().stride != descriptorBytes ||
      keypointFrame.ref().stride != kKeypointBytes) {
    return Err<FeatureSet>(StatusCode::Internal, kComponent,
                           "the store padded the rows of a frame these descriptors must be packed "
                           "into");
  }

  for (int32_t row = 0; row < features.count; ++row) {
    std::memcpy(descriptorSpan.value.data() + static_cast<size_t>(row) * descriptorBytes,
                descriptors.ptr(row), static_cast<size_t>(descriptorBytes));
    const float xy[2] = {keypoints[row].pt.x, keypoints[row].pt.y};
    std::memcpy(keypointSpan.value.data() + static_cast<size_t>(row) * kKeypointBytes, xy,
                sizeof(xy));
  }

  features.descriptors = descriptorFrame.ref();
  features.keypoints = keypointFrame.ref();
  descriptorFrame.Commit();
  keypointFrame.Commit();
  return Ok(features);
}

Result<FeatureSet> FeatureRegistrationEngine::ExtractFeatures(const FrameRef& frame) {
  // **The exception boundary ADR 0047 named and left for the ADR introducing the first such engine
  // to build.** OpenCV reports ordinary failure by throwing `cv::Exception` — `cv::Mat` asserts its
  // step against the row, and every `CV_Assert` in `features2d` behaves the same way — while the
  // core is compiled `-fno-exceptions` and every contract here promises a `Result`. This one
  // translation unit takes `-fexceptions` back (see `core/CMakeLists.txt`) and converts at its own
  // edge, which is what the layer rules already ask of a component adapting something foreign.
  //
  // The guards in `Extract` are not made redundant by this and are not a duplicate of it. They
  // answer the two cases we know about with `InvalidArgument` and a sentence naming what was wrong;
  // this answers the ones we do not, and can only say `Internal` and repeat OpenCV's text. A
  // refusal a caller can branch on is worth more than a catch-all — and the catch-all is worth
  // having because the list of things OpenCV asserts is not ours to know.
  try {
    return Extract(frame);
  } catch (const cv::Exception& e) {
    return Err<FeatureSet>(StatusCode::Internal, kComponent,
                           std::string("OpenCV refused this frame: ") + e.what());
  } catch (const std::exception& e) {
    return Err<FeatureSet>(StatusCode::Internal, kComponent,
                           std::string("feature extraction failed: ") + e.what());
  }
}

Result<PairwiseResult> FeatureRegistrationEngine::EstimatePairwise(const FeatureSet&,
                                                                  const FeatureSet&,
                                                                  const Quat&) {
  return Err<PairwiseResult>(StatusCode::Unsupported, kComponent,
                             "matching is the next increment; refusing rather than returning an "
                             "identity rotation that would look like a registration");
}

Result<GlobalSolution> FeatureRegistrationEngine::Refine(std::span<const PairwiseResult>,
                                                        std::span<const PoseSample>,
                                                        const Intrinsics&) {
  return Err<GlobalSolution>(StatusCode::Unsupported, kComponent,
                             "the global refinement is a later increment");
}

}  // namespace sphanorama

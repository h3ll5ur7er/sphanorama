#include "engines/registration_engine/feature_registration_engine.h"

#include <opencv2/features2d.hpp>
#include <opencv2/imgproc.hpp>

#include <cstring>
#include <vector>

namespace sphanorama {
namespace {

constexpr const char* kComponent = "FeatureRegistrationEngine";

// x and y as two float32s. The smallest thing a matcher needs, and the reason the keypoint frame
// exists separately from the descriptor frame: their row widths differ per detector.
constexpr int32_t kKeypointBytes = 8;

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
    case FeatureDetector::Orb:   return cv::ORB::create();
    case FeatureDetector::Akaze: return cv::AKAZE::create();
    case FeatureDetector::Sift:  return cv::SIFT::create();
  }
  return {};
}

// The luma plane as a single-channel Mat, without copying where the layout already allows it.
cv::Mat LumaOf(const FrameRef& frame, std::span<uint8_t> bytes) {
  switch (frame.format) {
    case PixelFormat::Gray8:
    case PixelFormat::NV12:
    case PixelFormat::I420:
      // All three lead with a full-resolution luma plane, which is all a detector reads.
      return cv::Mat(frame.height, frame.width, CV_8UC1, bytes.data(), frame.stride);
    case PixelFormat::RGBA8:
    case PixelFormat::BGRA8: {
      const cv::Mat colour(frame.height, frame.width, CV_8UC4, bytes.data(), frame.stride);
      cv::Mat grey;
      cv::cvtColor(colour, grey,
                   frame.format == PixelFormat::RGBA8 ? cv::COLOR_RGBA2GRAY : cv::COLOR_BGRA2GRAY);
      return grey;
    }
    default:
      return {};
  }
}

}  // namespace

Result<FeatureSet> FeatureRegistrationEngine::ExtractFeatures(const FrameRef& frame) {
  if (!HasReadableLuma(frame.format)) {
    return Err<FeatureSet>(StatusCode::Unsupported, kComponent,
                           "this frame has no luma plane to read; an encoded frame needs decoding "
                           "before features can be found in it");
  }
  if (frame.width <= 0 || frame.height <= 0) {
    return Err<FeatureSet>(StatusCode::InvalidArgument, kComponent, "a frame with no pixels");
  }

  auto pinned = frames_.Pin(frame);
  if (!pinned.ok()) return pinned.status;

  // Released on every path out, failures included. Pin promises the mapping until Release, and an
  // engine that kept one would pin a frame per extraction until the session ended.
  struct Unpin {
    IFrameStoreAccess& frames;
    const FrameRef& frame;
    ~Unpin() { (void)frames.Release(frame); }
  } unpin{frames_, frame};

  const cv::Mat luma = LumaOf(frame, pinned.value);
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

  FeatureSet features;
  features.frame = frame.id;
  features.count = static_cast<int32_t>(keypoints.size());

  // A frame with nothing in it is answered with nothing, rather than with two empty allocations
  // the caller would then have to remember to forget. `count == 0` is the whole answer.
  if (features.count == 0) return Ok(features);

  const int32_t descriptorBytes = static_cast<int32_t>(descriptors.cols * descriptors.elemSize());

  auto descriptorFrame = frames_.Allocate(descriptorBytes, features.count, PixelFormat::Gray8);
  if (!descriptorFrame.ok()) return descriptorFrame.status;

  auto keypointFrame = frames_.Allocate(kKeypointBytes, features.count, PixelFormat::Gray8);
  if (!keypointFrame.ok()) {
    // The descriptor frame is this engine's, not the caller's, and nobody else has seen it — so
    // forgetting it here drops nothing anyone could still want. That is the distinction this
    // codebase learned the hard way: a rollback is safe when it unwinds what the failing call
    // itself created, and dangerous when it unwinds what a caller handed over.
    (void)frames_.Forget(descriptorFrame.value);
    return keypointFrame.status;
  }

  auto descriptorBytes_ = frames_.Pin(descriptorFrame.value);
  auto keypointBytes = frames_.Pin(keypointFrame.value);
  if (!descriptorBytes_.ok() || !keypointBytes.ok()) {
    if (descriptorBytes_.ok()) (void)frames_.Release(descriptorFrame.value);
    if (keypointBytes.ok()) (void)frames_.Release(keypointFrame.value);
    (void)frames_.Forget(descriptorFrame.value);
    (void)frames_.Forget(keypointFrame.value);
    return descriptorBytes_.ok() ? keypointBytes.status : descriptorBytes_.status;
  }

  // The store returned frames of the size asked for, and this checks rather than assumes it. A
  // `memcpy` loop sized by `count` into a span sized by whatever came back is the shape that
  // overflows silently — found by a sabotage that shrank the allocation by one row and killed the
  // test binary outright, which under a harness reading gtest's summary looks exactly like a
  // sabotage that changed nothing. With this, the same sabotage fails cleanly instead.
  const size_t descriptorNeeded = static_cast<size_t>(features.count) * descriptorBytes;
  const size_t keypointNeeded = static_cast<size_t>(features.count) * kKeypointBytes;
  if (descriptorBytes_.value.size() < descriptorNeeded ||
      keypointBytes.value.size() < keypointNeeded) {
    (void)frames_.Release(descriptorFrame.value);
    (void)frames_.Release(keypointFrame.value);
    (void)frames_.Forget(descriptorFrame.value);
    (void)frames_.Forget(keypointFrame.value);
    return Err<FeatureSet>(StatusCode::Internal, kComponent,
                           "the store returned a frame smaller than the one asked for");
  }

  for (int32_t row = 0; row < features.count; ++row) {
    std::memcpy(descriptorBytes_.value.data() + static_cast<size_t>(row) * descriptorBytes,
                descriptors.ptr(row), static_cast<size_t>(descriptorBytes));
    const float xy[2] = {keypoints[row].pt.x, keypoints[row].pt.y};
    std::memcpy(keypointBytes.value.data() + static_cast<size_t>(row) * kKeypointBytes, xy,
                sizeof(xy));
  }

  (void)frames_.Release(descriptorFrame.value);
  (void)frames_.Release(keypointFrame.value);

  features.descriptors = descriptorFrame.value;
  features.keypoints = keypointFrame.value;
  return Ok(features);
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

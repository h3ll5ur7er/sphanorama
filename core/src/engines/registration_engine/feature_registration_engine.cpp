#include "engines/registration_engine/feature_registration_engine.h"

#include <opencv2/core.hpp>
#include <opencv2/features2d.hpp>
#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <exception>
#include <limits>
#include <string>
#include <vector>

#include "utilities/camera_model.h"
#include "utilities/pixel_format.h"
#include "utilities/quaternion.h"

namespace sphanorama {
namespace {

constexpr const char* kComponent = "FeatureRegistrationEngine";

// x and y as two float32s. The smallest thing a matcher needs, and the reason the keypoint frame
// exists separately from the descriptor frame: their row widths differ per detector.
constexpr int32_t kKeypointBytes = 8;

// `kMaxFeaturesPerFrame` is in the header, because a test has to build the same detector with the
// same cap to check this engine against. Measured on this repository's own test texture at
// 2048x2048, uncapped: SIFT returns 7,567 (3.87 MB of descriptors for one frame, 232 MB over the
// sixty a sphere plans) and AKAZE returns 14,656 — decimal megabytes, as ADR 0051 counts them. A
// capture frame is several times that again in pixels.
//
// ORB used to be listed beside them as "flatlines at 500", which was its own default being reported
// back as though it were a measurement: there is no uncapped ORB to take. `cv::ORB::create()`
// defaults `nfeatures` to 500 and `create(0)` means *zero* rather than unlimited, so the only way to
// ask is to name a big number — and the number you get back depends on how big. Asked for a million
// it answers 622,433 on that texture; the count is still climbing there, and saturates at 693,641
// from two million upward. The first version of this line quoted the 622,433 alone as though it
// were the ceiling, which is the same mistake one step out from the one this paragraph exists to
// correct: a figure that is an artefact of what was asked, reported as a property of the detector.

/**
 * How a format carries its luma, and the one place that is decided.
 *
 * There were three copies of this fact: this list, `LumaOf`'s switch over the same five formats,
 * and a `luma.empty()` check after the call. A reviewer showed the third could never fire — the
 * only way `LumaOf` answers empty is a format this function has already refused — which is what a
 * fact held in three places looks like just before one of them drifts. Deriving the other two from
 * this leaves one list to get wrong.
 */
enum class LumaKind { None, Planar, FourChannel };

LumaKind LumaKindOf(PixelFormat format) {
  switch (format) {
    case PixelFormat::Gray8:
    case PixelFormat::NV12:
    case PixelFormat::I420:
      // All three lead with a full-resolution luma plane, which is all a detector reads.
      return LumaKind::Planar;
    case PixelFormat::RGBA8:
    case PixelFormat::BGRA8:
      return LumaKind::FourChannel;
    default:
      return LumaKind::None;
  }
}

bool HasReadableLuma(PixelFormat format) { return LumaKindOf(format) != LumaKind::None; }

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
    case FeatureDetector::Count:
      // Not a detector — the header says why it exists. Answering null here sends it down the
      // "no such detector" refusal that already existed, rather than inventing a second one.
      break;
  }
  return {};
}

/**
 * Bytes one pixel occupies in the row this engine reads.
 *
 * Derived from `LumaKindOf` rather than from `BytesPerPixel`, so the width the bounds below are
 * computed from and the width the read actually steps come from **one** authority. They agreed
 * before — 4 for the two four-channel formats, and 1 for the three planar ones, where
 * `BytesPerPixel` answers 0 and a `std::max(..., 1)` floor rescued it — but nothing made them, and
 * a reviewer named the shape that breaks it: a packed 4:2:2 format handled here as a new kind
 * stepping two bytes, while `BytesPerPixel` goes on answering 0 and the floor makes it 1. Every
 * guard would then pass on a span half the size the read needs.
 */
int64_t LumaRowBytesPerPixel(PixelFormat format) {
  switch (LumaKindOf(format)) {
    case LumaKind::FourChannel:
      return 4;
    case LumaKind::Planar:
      return 1;
    case LumaKind::None:
      break;
  }
  return 0;
}

// The luma plane as a single-channel Mat, without copying where the layout already allows it.
cv::Mat LumaOf(const FrameRef& frame, std::span<uint8_t> bytes, size_t stride) {
  switch (LumaKindOf(frame.format)) {
    case LumaKind::Planar:
      return cv::Mat(frame.height, frame.width, CV_8UC1, bytes.data(), stride);
    case LumaKind::FourChannel: {
      const cv::Mat colour(frame.height, frame.width, CV_8UC4, bytes.data(), stride);
      cv::Mat grey;
      cv::cvtColor(colour, grey,
                   frame.format == PixelFormat::RGBA8 ? cv::COLOR_RGBA2GRAY : cv::COLOR_BGRA2GRAY);
      return grey;
    }
    case LumaKind::None:
      // `Extract` refuses this format before pinning, so this arm is how the switch stays total
      // rather than a state anything reaches.
      break;
  }
  return {};
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
  const int64_t bytesPerPixel = LumaRowBytesPerPixel(frame.format);
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
  if (rows > 0) {
    // Exact, and the only check for a frame of more than one row: `stride <= (held - rowBytes) /
    // rows` is `rows * stride + rowBytes <= held` for positive `rows`, without ever forming the
    // product. A claim wider than the whole allocation makes the numerator negative, and every
    // stride is greater than a negative, so it is refused by the same line rather than a second one.
    if (stride > (held - rowBytes) / rows) {
      return Err<FeatureSet>(StatusCode::InvalidArgument, kComponent,
                             "this frame claims more pixels than the store is holding for it");
    }
  } else if (rowBytes > held) {
    // One row, so there is no product to bound and the division above is skipped. This is the only
    // thing standing between a single enormous row and the span it would be read from.
    return Err<FeatureSet>(StatusCode::InvalidArgument, kComponent,
                           "this frame claims more pixels than the store is holding for it");
  }
  // A planar frame's buffer holds chroma after the luma plane, so the rows above bound the *bytes*
  // and not the *picture*: a real I420 128x128 is 24,576 bytes, and a handle claiming 192 rows needs
  // exactly that by luma arithmetic alone. The whole-frame size is what says how much of it is
  // picture, and it is in bounds either way — so ASan never had anything to say about it.
  if (BytesPerPixel(frame.format) <= 0) {
    // `FrameByteSize` counts *packed* bytes while the rows above are counted in *strided* ones, so
    // a claim whose stride exceeds its width satisfies both at once: narrowing the claimed width
    // buys packed budget to pay for extra rows. On a real packed I420 640x480, the handle
    // {width 400, height 720, stride 640} passes every other check here and hands a detector 240
    // rows of chroma as picture. Requiring the rows to be packed is what closes it, and it refuses
    // nothing real: `MemoryFrameStoreAccess::Allocate` packs planar rows.
    //
    // Besides `Allocate`, a stride is written by the generated wire decoder — in both halves,
    // `codec.h` and `shell/src/bridge/codec.generated.ts` — and by `CaptureSessionManager`'s
    // `DecodeSession`, which reads one out of a persisted project document with `operator>>` and
    // hands the result to an engine. That last one is why this is a guard and not an assertion: a
    // stride can arrive from a file written by an older build.
    //
    // The count has been wrong in four successive drafts of this comment — one writer, then two,
    // then three, now four — each corrected by someone who read further than the last. It is left
    // unstated on purpose now: an exhaustive count in a comment is a claim that rots the next time
    // anyone adds a decoder, and the reason for the guard does not depend on the total.
    if (stride != rowBytes) {
      return Err<FeatureSet>(StatusCode::InvalidArgument, kComponent,
                             "a planar frame's rows must be packed; this one claims a stride wider "
                             "than its width, which would hide chroma behind the claim");
    }
    const int64_t whole = FrameByteSize(frame.width, frame.height, frame.format);
    if (whole <= 0 || whole > held) {
      return Err<FeatureSet>(StatusCode::InvalidArgument, kComponent,
                             "this frame claims more rows of picture than the store is holding for "
                             "it; the bytes past the luma plane are chroma");
    }
  }

  // No `empty()` check: `HasReadableLuma` above and this call now read the same list, so the only
  // format that could produce an empty `Mat` is one that was refused before the frame was pinned.
  const cv::Mat luma = LumaOf(frame, pinned.value, static_cast<size_t>(stride));

  cv::Ptr<cv::Feature2D> detector = Make(detector_);
  if (!detector) {
    return Err<FeatureSet>(StatusCode::Unsupported, kComponent, "no such detector");
  }

  std::vector<cv::KeyPoint> keypoints;
  cv::Mat descriptors;
  detector->detectAndCompute(luma, cv::noArray(), keypoints, descriptors);

  // **Asking for a cap is not the same as getting one**, and the surplus is not what two earlier
  // versions of this comment claimed.
  //
  // SIFT returned 501 for a request of 500 on this repository's texture at 768 square, because
  // `KeyPointsFilter::retainBest` keeps everyone tied with the last of its selection — so for SIFT
  // the overflow really is the boundary ties. ORB is a different mechanism entirely: `orb.cpp`
  // applies `retainBest(keypoints, featuresNum)` **per pyramid level** and concatenates the levels,
  // so the total is capped nowhere, the order is level-major rather than response-major, and the
  // surplus is whole octaves. `Ruled` is the fixture that shows it: asked for 500 at 768 square, ORB
  // returns **1,145**, which is the number the cap test below is built on.
  //
  // An earlier version of this line cited "772 on a tie-rich image", from a review that named no
  // fixture, size or configuration. It reproduces at none of them — a swept measurement nobody can
  // re-run is worth less than no number at all, and it sat two lines from one that is anchored to a
  // fixture in this repository.
  //
  // The overrun is also not monotonic, which is the part that makes reasoning about it a mistake
  // rather than merely hard: the same `Ruled` pattern at 2048 square returns **395** for a request
  // of 500 — *under* the cap, because the per-level budgets are computed from the pyramid and not
  // from what the image can supply.
  //
  // Neither is a safe thing to reason about, so the code stops reasoning about it. Rather than keep
  // the first `n` and argue about what the detector put there, it keeps the best `n` by response —
  // which is true by construction for every detector, present and future, and makes `FeatureSet`'s
  // rows best-first.
  //
  // What that order is *not* is a spread across scales, and this comment used to end by telling a
  // matcher it was what one taking a top-`k` wanted anyway. ORB's response is a Harris score
  // computed per pyramid level and never normalised between them, so the strongest rows are nearly
  // all one octave — measured at 48 of the top 50 on this repository's texture. `FeatureSet`'s
  // comment says so, because it is the caller who would otherwise assume otherwise.
  //
  // **Stable rather than merely deterministic.** An earlier version of this comment gave
  // repeatability as the reason, which is a property `std::sort` has just as fully. What a plain
  // sort would not keep is the detector's own order among equal responses — and that tie order is a
  // promise `FeatureSet` makes to its caller. It is pinned rather than asserted: on a fixture where
  // responses tie in quantity, `std::sort` here fails the cap test on all three detectors.
  //
  // Always, not only when the cap bites: an order that changes shape at 500 features is a promise
  // nobody can state, and a caller would have no way to know which one it got.
  std::vector<int> order(keypoints.size());
  for (size_t at = 0; at < order.size(); ++at) order[at] = static_cast<int>(at);
  std::stable_sort(order.begin(), order.end(), [&keypoints](int a, int b) {
    return keypoints[static_cast<size_t>(a)].response > keypoints[static_cast<size_t>(b)].response;
  });
  if (order.size() > static_cast<size_t>(kMaxFeaturesPerFrame)) {
    order.resize(static_cast<size_t>(kMaxFeaturesPerFrame));
  }

  FeatureSet features;
  features.frame = frame.id;
  features.count = static_cast<int32_t>(order.size());

  // A frame with nothing in it is answered with nothing, rather than with two empty allocations
  // the caller would then have to remember to forget. `count == 0` is the whole answer.
  if (features.count == 0) return Ok(features);

  // The detector's two outputs against each other, before `count` is used to index either. The
  // loop below reads `descriptors.ptr(row)` for every row it copies, and `cv::Mat::ptr(int)` only
  // `CV_DbgAssert`s its argument — so a detector returning fewer descriptor rows than keypoints
  // would read past the Mat with no diagnostic in a release build. No detector here disagrees
  // today; this is the same assumption the code below explicitly refuses to make about the store,
  // and it costs one comparison to stop making it here too.
  if (descriptors.rows < static_cast<int>(keypoints.size())) {
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
    // Through `order`, so a descriptor stays with its keypoint. The two are row-aligned as the
    // detector produced them, and selecting the best `n` reorders both or neither.
    const int from = order[static_cast<size_t>(row)];
    std::memcpy(descriptorSpan.value.data() + static_cast<size_t>(row) * descriptorBytes,
                descriptors.ptr(from), static_cast<size_t>(descriptorBytes));
    const float xy[2] = {keypoints[static_cast<size_t>(from)].pt.x,
                         keypoints[static_cast<size_t>(from)].pt.y};
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
  // answer the cases we know about with `InvalidArgument` and a sentence naming what was wrong; this
  // answers OpenCV's own refusals, and can only say `Internal` and repeat its text. A refusal a
  // caller can branch on is worth more than a catch-all — and the catch-all is worth having because
  // the list of things OpenCV asserts is not ours to know.
  //
  // "The ones we do not know about" would be claiming more than these handlers do. They catch what
  // derives from `std::exception`, which is what OpenCV throws; a `catch (...)` would cover the rest
  // and is deliberately absent, because there is nothing useful to say about a throw of unknown type
  // and swallowing it would hide a fault this engine cannot describe.
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

namespace {

/**
 * A frame this call was *handed*, pinned for the length of the call and given back — never
 * forgotten.
 *
 * `OwnedFrame` above is the wrong shape here and the difference is the whole reason this exists:
 * that one forgets unless committed, which is right for a frame the engine allocated and is about
 * to hand over. The four frames reaching `EstimatePairwise` belong to the caller, who may estimate
 * the same pair twice or one set against several others, so an implementation that tidied up after
 * itself would destroy the second call's input. The contract says so in as many words, and it says
 * so because a reviewer noticed the ownership rule had been written for the frames coming *out* of
 * `ExtractFeatures` with nothing said about the four going in.
 */
class BorrowedFrame {
 public:
  BorrowedFrame(IFrameStoreAccess& frames, const FrameRef& frame) : frames_(frames), frame_(frame) {}
  ~BorrowedFrame() {
    if (pinned_) (void)frames_.Release(frame_);
  }
  BorrowedFrame(const BorrowedFrame&) = delete;
  BorrowedFrame& operator=(const BorrowedFrame&) = delete;

  Result<std::span<uint8_t>> Pin() {
    Result<std::span<uint8_t>> pinned = frames_.Pin(frame_);
    if (pinned.ok()) pinned_ = true;
    return pinned;
  }

 private:
  IFrameStoreAccess& frames_;
  FrameRef frame_;
  bool pinned_ = false;
};

/** The bearing a keypoint names, or nothing — `Unproject` refuses rather than guessing (ADR 0046). */
struct Bearing {
  Vec3 direction;
  Pixel pixel;
};

/** A rotation as a matrix, so the prior can be scored by the same code path as a sampled one. */
cv::Matx33d RotationMatrix(const Quat& q) {
  const double n = std::sqrt(q.w * q.w + q.x * q.x + q.y * q.y + q.z * q.z);
  const double w = q.w / n, x = q.x / n, y = q.y / n, z = q.z / n;
  return cv::Matx33d(1 - 2 * (y * y + z * z), 2 * (x * y - z * w),     2 * (x * z + y * w),
                     2 * (x * y + z * w),     1 - 2 * (x * x + z * z), 2 * (y * z - x * w),
                     2 * (x * z - y * w),     2 * (y * z + x * w),     1 - 2 * (x * x + y * y));
}

Quat FromMatrix(const cv::Matx33d& r) {
  // Shepperd's method by the largest denominator, rather than the textbook `w`-first form: taking
  // the square root of a near-zero `1 + trace` loses most of the mantissa for rotations near a half
  // turn, which a ring of frames reaches.
  const double trace = r(0, 0) + r(1, 1) + r(2, 2);
  Quat q{};
  if (trace > 0.0) {
    const double s = std::sqrt(trace + 1.0) * 2.0;
    q.w = 0.25 * s;
    q.x = (r(2, 1) - r(1, 2)) / s;
    q.y = (r(0, 2) - r(2, 0)) / s;
    q.z = (r(1, 0) - r(0, 1)) / s;
  } else if (r(0, 0) > r(1, 1) && r(0, 0) > r(2, 2)) {
    const double s = std::sqrt(1.0 + r(0, 0) - r(1, 1) - r(2, 2)) * 2.0;
    q.w = (r(2, 1) - r(1, 2)) / s;
    q.x = 0.25 * s;
    q.y = (r(0, 1) + r(1, 0)) / s;
    q.z = (r(0, 2) + r(2, 0)) / s;
  } else if (r(1, 1) > r(2, 2)) {
    const double s = std::sqrt(1.0 + r(1, 1) - r(0, 0) - r(2, 2)) * 2.0;
    q.w = (r(0, 2) - r(2, 0)) / s;
    q.x = (r(0, 1) + r(1, 0)) / s;
    q.y = 0.25 * s;
    q.z = (r(1, 2) + r(2, 1)) / s;
  } else {
    const double s = std::sqrt(1.0 + r(2, 2) - r(0, 0) - r(1, 1)) * 2.0;
    q.w = (r(1, 0) - r(0, 1)) / s;
    q.x = (r(0, 2) + r(2, 0)) / s;
    q.y = (r(1, 2) + r(2, 1)) / s;
    q.z = 0.25 * s;
  }
  return q;
}

/**
 * The rotation carrying `from` onto `to`, in the least-squares sense — Kabsch by SVD.
 *
 * Returns false when the set is degenerate: fewer than two bearings, or bearings so nearly parallel
 * that the covariance is rank-deficient and the rotation about their common axis is unconstrained.
 * A rotation fitted to a degenerate set is not a bad estimate, it is an arbitrary one, and the
 * difference matters because the caller cannot tell them apart from the quaternion.
 */
bool KabschRotation(const std::vector<Vec3>& from, const std::vector<Vec3>& to, cv::Matx33d* out) {
  if (from.size() < 2 || from.size() != to.size()) return false;
  cv::Matx33d covariance = cv::Matx33d::zeros();
  for (size_t at = 0; at < from.size(); ++at) {
    const Vec3& f = from[at];
    const Vec3& t = to[at];
    covariance += cv::Matx33d(t.x * f.x, t.x * f.y, t.x * f.z,
                              t.y * f.x, t.y * f.y, t.y * f.z,
                              t.z * f.x, t.z * f.y, t.z * f.z);
  }
  cv::Matx33d u, vt;
  cv::Matx31d w;
  cv::SVD::compute(covariance, w, u, vt);
  // Two bearings span a plane, so the third singular value is legitimately zero there; it is the
  // *second* going to zero that means the bearings are parallel and the fit is unconstrained.
  if (!(w(1, 0) > 1e-9)) return false;
  cv::Matx33d r = u * vt;
  if (cv::determinant(r) < 0) {
    // A reflection fits the points as well as a rotation does and is not one. Flipping the sign of
    // the column matched to the smallest singular value is the least-damaging repair.
    cv::Matx33d flip = cv::Matx33d::eye();
    flip(2, 2) = -1;
    r = u * flip * vt;
  }
  *out = r;
  return true;
}

Vec3 Rotate(const cv::Matx33d& r, const Vec3& v) {
  return Vec3{r(0, 0) * v.x + r(0, 1) * v.y + r(0, 2) * v.z,
              r(1, 0) * v.x + r(1, 1) * v.y + r(1, 2) * v.z,
              r(2, 0) * v.x + r(2, 1) * v.y + r(2, 2) * v.z};
}

// Lowe's 0.75. The value is his and the reason it is not tuned here is that tuning it against one
// synthetic dataset would fit it to a checkerboard.
constexpr double kLoweRatio = 0.75;
// Two bearings determine a rotation, so this is not the algebraic minimum — it is the point below
// which RANSAC has no outlier to reject and the answer is whatever the two points say.
constexpr size_t kMinimumCorrespondences = 8;
// The inlier gate, in pixels of reprojection error. Generous on purpose, per the skill's advice for
// a first bound: a bound that exists and is loose beats a precise one that does not.
constexpr double kInlierPx = 3.0;
constexpr int kRansacIterations = 200;

/**
 * The keypoint rows of a set, as directions.
 *
 * Eight bytes a row, two little-endian `float32`s, x then y — the contract spells that out because
 * it used to be a constant in this file's anonymous namespace and nowhere a caller could read it.
 * A row `Unproject` refuses is dropped rather than guessed at: the refusals it can answer are a
 * direction past the fold or one that does not land back where it started, and both would put a
 * feature in the wrong place while the file looked well-formed.
 */
Result<std::vector<Bearing>> ReadBearings(const FeatureSet& set, std::span<uint8_t> rows,
                                          const Intrinsics& lens) {
  const size_t needed = static_cast<size_t>(set.count) * static_cast<size_t>(kKeypointBytes);
  if (rows.size() < needed) {
    return Err<std::vector<Bearing>>(StatusCode::InvalidArgument, kComponent,
                                     "the keypoint frame holds fewer bytes than its own row count "
                                     "needs");
  }
  std::vector<Bearing> bearings;
  bearings.reserve(static_cast<size_t>(set.count));
  for (int32_t row = 0; row < set.count; ++row) {
    float xy[2] = {0, 0};
    std::memcpy(xy, rows.data() + static_cast<size_t>(row) * kKeypointBytes, sizeof(xy));
    const Pixel pixel{static_cast<double>(xy[0]), static_cast<double>(xy[1])};
    const UnprojectedDirection unprojected = Unproject(lens, pixel);
    if (!unprojected.valid) continue;
    bearings.push_back(Bearing{unprojected.direction, pixel});
  }
  return Ok(std::move(bearings));
}

/** The descriptor rows as a `cv::Mat` over the pinned bytes — a view, copied by nothing. */
Result<cv::Mat> ReadDescriptors(const FeatureSet& set, std::span<uint8_t> rows) {
  if (set.count <= 0 || rows.empty()) {
    return Err<cv::Mat>(StatusCode::InvalidArgument, kComponent, "no descriptor rows to read");
  }
  const size_t width = rows.size() / static_cast<size_t>(set.count);
  if (width == 0) {
    return Err<cv::Mat>(StatusCode::InvalidArgument, kComponent,
                        "the descriptor frame holds fewer bytes than one row per feature");
  }
  // SIFT's are 128 `float32`s and the binary detectors' are bytes; the width in bytes tells them
  // apart, because only the float one is divisible by four *and* four times a plausible row count.
  if (width % sizeof(float) == 0 && width >= 128 * sizeof(float)) {
    return Ok(cv::Mat(set.count, static_cast<int>(width / sizeof(float)), CV_32F, rows.data()));
  }
  return Ok(cv::Mat(set.count, static_cast<int>(width), CV_8U, rows.data()));
}

/**
 * The rotation most of the correspondences agree on, found by RANSAC and refitted on its inliers.
 *
 * The prior is a hypothesis rather than a starting point to descend from: it is scored alongside the
 * sampled ones, so a sensor that is right wins immediately and a sensor that is wrong loses to the
 * pixels. That is what "seeds and bounds, never truth" has to mean for the estimate to be able to
 * disagree with the sensor.
 */
Result<PairwiseResult> FitRotation(const std::vector<Vec3>& from, const std::vector<Vec3>& to,
                                   const std::vector<Pixel>& observed, const Quat& prior,
                                   const Intrinsics& lens) {
  const auto residualPx = [&](const cv::Matx33d& r, size_t at) -> double {
    const ProjectedPixel landed = Project(lens, Rotate(r, from[at]));
    if (!landed.valid) return std::numeric_limits<double>::infinity();
    const double dx = landed.pixel.x - observed[at].x;
    const double dy = landed.pixel.y - observed[at].y;
    return std::sqrt(dx * dx + dy * dy);
  };
  const auto countInliers = [&](const cv::Matx33d& r, std::vector<size_t>* keep) {
    keep->clear();
    for (size_t at = 0; at < from.size(); ++at) {
      if (residualPx(r, at) <= kInlierPx) keep->push_back(at);
    }
  };

  cv::Matx33d best = RotationMatrix(prior);
  std::vector<size_t> bestInliers;
  countInliers(best, &bestInliers);

  // Deterministic, and that is a property the selection tests rely on elsewhere in this core: the
  // same correspondences and the same prior must give the same rotation, so the sampling walks a
  // fixed stride rather than a clock-seeded generator.
  std::vector<size_t> candidate;
  for (int iteration = 0; iteration < kRansacIterations; ++iteration) {
    const size_t i = static_cast<size_t>(iteration * 7 + 1) % from.size();
    const size_t j = static_cast<size_t>(iteration * 13 + 5) % from.size();
    const size_t k = static_cast<size_t>(iteration * 23 + 11) % from.size();
    if (i == j || j == k || i == k) continue;
    cv::Matx33d sampled;
    if (!KabschRotation({from[i], from[j], from[k]}, {to[i], to[j], to[k]}, &sampled)) continue;
    countInliers(sampled, &candidate);
    if (candidate.size() > bestInliers.size()) {
      best = sampled;
      bestInliers = candidate;
    }
  }

  if (bestInliers.size() < kMinimumCorrespondences) {
    return Err<PairwiseResult>(StatusCode::NotFound, kComponent,
                               "no rotation was agreed on by enough correspondences: the best had " +
                                   std::to_string(bestInliers.size()));
  }

  // Refit on every inlier, which is what makes the answer better than the three points that found
  // it, and re-gate: the refit moves the rotation, so its inlier set is not the one it was fitted
  // from and reporting the old count would overstate the agreement.
  std::vector<Vec3> inFrom;
  std::vector<Vec3> inTo;
  for (size_t at : bestInliers) {
    inFrom.push_back(from[at]);
    inTo.push_back(to[at]);
  }
  cv::Matx33d refined = best;
  if (KabschRotation(inFrom, inTo, &refined)) {
    std::vector<size_t> after;
    countInliers(refined, &after);
    if (after.size() >= bestInliers.size()) {
      best = refined;
      bestInliers = after;
    }
  }

  std::vector<double> residuals;
  residuals.reserve(bestInliers.size());
  for (size_t at : bestInliers) residuals.push_back(residualPx(best, at));
  std::sort(residuals.begin(), residuals.end());
  const double median = residuals.empty() ? 0.0 : residuals[residuals.size() / 2];

  PairwiseResult answer{};
  answer.relativeRotation = FromMatrix(best);
  answer.inliers = static_cast<int32_t>(bestInliers.size());
  answer.medianResidualPx = median;
  answer.accepted = bestInliers.size() >= kMinimumCorrespondences && median <= kInlierPx;
  return Ok(answer);
}


}  // namespace

Result<PairwiseResult> FeatureRegistrationEngine::EstimatePairwise(const FeatureSet& a,
                                                                  const FeatureSet& b,
                                                                  const Quat& prior,
                                                                  const Intrinsics& lens) {
  if (a.count <= 0 || b.count <= 0) {
    return Err<PairwiseResult>(StatusCode::InvalidArgument, kComponent,
                               "a feature set with no rows cannot be matched against anything");
  }
  // The prior seeds and bounds the search, so an unusable one is a refusal rather than a silent
  // fall back to identity — identity *is* a rotation, and a caller handed one would read a failed
  // seeding as a frame that had not moved.
  if (!IsUsableRotation(prior)) {
    return Err<PairwiseResult>(StatusCode::InvalidArgument, kComponent,
                               "the sensor prior is not a usable rotation");
  }
  if (!IsUsableLens(lens)) {
    return Err<PairwiseResult>(StatusCode::InvalidArgument, kComponent,
                               "the lens cannot project, so no rotation can be recovered from "
                               "pixels (ADR 0054)");
  }

  try {
    // Borrowed, not owned: pinned for the call and released by these destructors on every path out,
    // and never forgotten. See `BorrowedFrame`.
    BorrowedFrame keypointsA(frames_, a.keypoints);
    BorrowedFrame descriptorsA(frames_, a.descriptors);
    BorrowedFrame keypointsB(frames_, b.keypoints);
    BorrowedFrame descriptorsB(frames_, b.descriptors);

    const Result<std::span<uint8_t>> kaSpan = keypointsA.Pin();
    const Result<std::span<uint8_t>> daSpan = descriptorsA.Pin();
    const Result<std::span<uint8_t>> kbSpan = keypointsB.Pin();
    const Result<std::span<uint8_t>> dbSpan = descriptorsB.Pin();
    for (const Result<std::span<uint8_t>>* pinned : {&kaSpan, &daSpan, &kbSpan, &dbSpan}) {
      if (!pinned->ok()) return Err<PairwiseResult>(pinned->status.code, kComponent,
                                                    pinned->status.detail);
    }

    const Result<std::vector<Bearing>> bearingsA = ReadBearings(a, kaSpan.value, lens);
    if (!bearingsA.ok()) {
      return Err<PairwiseResult>(bearingsA.status.code, kComponent, bearingsA.status.detail);
    }
    const Result<std::vector<Bearing>> bearingsB = ReadBearings(b, kbSpan.value, lens);
    if (!bearingsB.ok()) {
      return Err<PairwiseResult>(bearingsB.status.code, kComponent, bearingsB.status.detail);
    }

    const Result<cv::Mat> matA = ReadDescriptors(a, daSpan.value);
    if (!matA.ok()) return Err<PairwiseResult>(matA.status.code, kComponent, matA.status.detail);
    const Result<cv::Mat> matB = ReadDescriptors(b, dbSpan.value);
    if (!matB.ok()) return Err<PairwiseResult>(matB.status.code, kComponent, matB.status.detail);
    if (matA.value.type() != matB.value.type() || matA.value.cols != matB.value.cols) {
      return Err<PairwiseResult>(StatusCode::InvalidArgument, kComponent,
                                 "the two feature sets were not made by the same detector");
    }

    // **Lowe's ratio test, two nearest neighbours.** A single nearest neighbour always exists, so
    // matching without the ratio produces a full set of correspondences for two frames of unrelated
    // scenery — the shape that makes a registration look successful and be nonsense.
    const int norm = matA.value.type() == CV_8U ? cv::NORM_HAMMING : cv::NORM_L2;
    cv::BFMatcher matcher(norm);
    std::vector<std::vector<cv::DMatch>> knn;
    matcher.knnMatch(matA.value, matB.value, knn, 2);

    std::vector<Vec3> fromAll;
    std::vector<Vec3> toAll;
    std::vector<Pixel> observed;
    for (const std::vector<cv::DMatch>& pair : knn) {
      if (pair.size() < 2) continue;
      if (pair[0].distance > kLoweRatio * pair[1].distance) continue;
      const size_t ia = static_cast<size_t>(pair[0].queryIdx);
      const size_t ib = static_cast<size_t>(pair[0].trainIdx);
      if (ia >= bearingsA.value.size() || ib >= bearingsB.value.size()) continue;
      fromAll.push_back(bearingsA.value[ia].direction);
      toAll.push_back(bearingsB.value[ib].direction);
      observed.push_back(bearingsB.value[ib].pixel);
    }

    if (fromAll.size() < kMinimumCorrespondences) {
      return Err<PairwiseResult>(StatusCode::NotFound, kComponent,
                                 "too few correspondences survived the ratio test to fit a "
                                 "rotation: " + std::to_string(fromAll.size()));
    }

    const Result<PairwiseResult> fitted =
        FitRotation(fromAll, toAll, observed, prior, lens);
    if (!fitted.ok()) return fitted;

    PairwiseResult answer = fitted.value;
    answer.a = a.frame;
    answer.b = b.frame;
    return Ok(answer);
  } catch (const cv::Exception& thrown) {
    return Err<PairwiseResult>(StatusCode::Internal, kComponent,
                               std::string("OpenCV refused during matching: ") + thrown.what());
  } catch (const std::exception& thrown) {
    return Err<PairwiseResult>(StatusCode::Internal, kComponent,
                               std::string("matching failed: ") + thrown.what());
  }
}

Result<GlobalSolution> FeatureRegistrationEngine::Refine(std::span<const PairwiseResult>,
                                                        std::span<const PoseSample>,
                                                        const Intrinsics&) {
  return Err<GlobalSolution>(StatusCode::Unsupported, kComponent,
                             "the global refinement is a later increment");
}

}  // namespace sphanorama

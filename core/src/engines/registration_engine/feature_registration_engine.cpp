#include "engines/registration_engine/feature_registration_engine.h"

#include <opencv2/core.hpp>
#include <opencv2/features2d.hpp>
#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <exception>
#include <cstdint>
#include <limits>
#include <numbers>
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

/**
 * The bearing a keypoint names, or nothing — `Unproject` refuses rather than guessing (ADR 0046).
 *
 * **`usable` rather than omitting the row, and this is a correctness fix rather than a style.** The
 * first version of `ReadBearings` skipped a refused row with `continue`, so the k-th element was the
 * k-th *accepted* keypoint while the matcher indexes this vector with *descriptor row* numbers. One
 * refused row at the top silently shifted every correspondence after it. A reviewer measured what
 * that costs: a perfect registration of sixty exact correspondences went from 0.0000 to 8.2257
 * degrees, reported with 43 inliers, a 1.62-pixel median, and `accepted` true.
 *
 * It was invisible here because every lens in these tests is distortion-free, so `Unproject` never
 * refuses. It is reachable in life: a 78-degree lens with `k1 = -0.20` refuses about 4% of in-frame
 * pixels, and a 100-degree one with `k1 = -0.35` refuses 38.8%.
 */
struct Bearing {
  Vec3 direction;
  Pixel pixel;
  bool usable = false;
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
  if (!BearingsSpanAPlane(w(0, 0), w(1, 0))) return false;
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
/**
 * The share of correspondences that must agree before an answer is `accepted`.
 *
 * **Set by the gap it has to straddle, not by what was measured.** A wrong correspondence lands
 * within `kInlierPx` of the right pixel by chance with probability about `pi * 3^2 / (640 * 480)`,
 * which is one in ten thousand — so agreement at any percentage at all is *structure* rather than
 * luck, and the question is how much structure to demand.
 *
 * The measurement it is checked against, taken by counting inliers under the *truth* rotation
 * rather than the estimated one: on a twelve-frame ring the correct rotation draws between 0.08 and
 * 0.38 of the correspondences, detector depending. (An earlier version of this paragraph said 0.31
 * to 0.46 and was quoting the arrangement that handed the estimator the exact truth as its prior —
 * so those were inlier counts of a rotation that had been given to it, on the pairs where it
 * answered at all. The low end is what that arrangement hid.)
 *
 * A fifth therefore sits inside the spread rather than below it, and that is the honest position:
 * it accepts the pairs a detector registers well and declines the ones where nine matches in ten
 * are wrong, which on this dataset is three of eleven ORB pairs — two of them answered and declined,
 * the third refused for want of any consensus at all. They are not a failure of the estimator: on
 * the two that answer, RANSAC returns as many inliers as the truth rotation itself does. Their
 * support really is a minority, and saying so is what `accepted` is for.
 *
 * **It is also the RANSAC search budget's floor**, so lowering it costs cubically: see
 * `RansacSampleBudget`. At 0.2 a call is about twenty milliseconds and at 0.01 it is fifteen
 * seconds.
 *
 * It is deliberately not set to the middle of the measured spread: a threshold set to the
 * observation is a threshold the next dataset moves. And it is not the gate that catches a
 * half-turn alias — those gather a real minority following, and the prior's bound is what excludes
 * them. Different failures, different gates.
 */
constexpr double kInlierFraction = 0.2;
/**
 * The confidence that the sampling loop draws at least one all-inlier triple.
 *
 * Turned into a number of draws by `RansacSampleBudget`, which is defined below the anonymous
 * namespace rather than "below" here — the name and the place in an earlier version of this line
 * were both wrong, which is a small thing that costs a reader a search.
 *
 * It is the real confidence rather than a nominal one: the loop draws its three indices without
 * replacement, so every iteration is a sample. While it drew independently and skipped collisions,
 * this said 0.99 and delivered 0.9515 at eight correspondences.
 */
constexpr double kRansacConfidence = 0.99;
// **How far the pixels may disagree with the sensor before the answer is a different scene rather
// than a wrong sensor.** This is the "bounds" half of the contract's "seeds and bounds, never
// truth", and leaving it out was not a simplification: on a panorama with a half-turn symmetry —
// a checkerboard, say — ORB and AKAZE match features to their point-reflected twins, and the
// aliased rotation *genuinely fits the pixels*, with ordinary inlier counts and sub-two-pixel
// residuals. Measured on a twelve-frame ring: two of eleven ORB steps and two of eleven AKAZE
// steps came back 175 to 179 degrees out, about the optical axis, and accepted.
//
// 45 degrees, and the number is chosen for its *shape* rather than fitted: a fused phone
// orientation is good to a few degrees when it is working and can be tens of degrees out after a
// magnetic disturbance, so the bound has to admit a badly drifted sensor. What it must exclude is
// not sensor error at all but a different interpretation of the scene, and those arrive near a
// half turn. Anything from roughly 60 to 150 degrees would have excluded the aliases seen here
// equally well, which is what makes this a policy and not a fit.
constexpr double kPriorBoundDeg = 45.0;

/**
 * The keypoint rows of a set, as directions.
 *
 * Eight bytes a row, two little-endian `float32`s, x then y — the contract spells that out because
 * it used to be a constant in this file's anonymous namespace and nowhere a caller could read it.
 * **A row `Unproject` refuses is marked, not dropped — and this sentence used to say the opposite.**
 * The refusals it can answer are a direction past the fold or one that does not land back where it
 * started, and skipping such a row shortens this vector while the descriptor rows the matcher
 * indexes it with stay where they are, so every bearing above the gap names a different feature —
 * and that feature's `pixel` too, which keeps the residual small and the wrong answer plausible.
 * Measured at 8.2257 degrees, reported with 43 inliers and `accepted` set. So the row stays in
 * place with `usable` false and the matcher skips the correspondence instead.
 *
 * The comment outlived the fix by a commit, which is its own lesson: a paragraph arguing for the
 * behaviour that was just removed is worse than no paragraph, because the next reader takes it as
 * the design and restores the bug.
 */
Result<std::vector<Bearing>> ReadBearings(const FeatureSet& set, std::span<uint8_t> rows,
                                          const Intrinsics& lens) {
  // **The frame's own stride, not the row size.** `Allocate` promises nothing about stride, which
  // is the argument `Extract` already makes when it refuses a padded keypoint frame — this read
  // assumed 8 and would have walked a padded frame diagonally.
  const int64_t pitch = set.keypoints.stride > 0 ? set.keypoints.stride : kKeypointBytes;
  if (pitch < kKeypointBytes) {
    return Err<std::vector<Bearing>>(StatusCode::InvalidArgument, kComponent,
                                     "the keypoint frame's stride is narrower than one row");
  }
  // Asked by division so the product cannot wrap before the check that would have refused it —
  // `size_t` is 32 bits on wasm32, where `count * pitch` overflows at a plausible count.
  if (set.count > 0 && static_cast<int64_t>(rows.size()) / pitch < set.count) {
    return Err<std::vector<Bearing>>(StatusCode::InvalidArgument, kComponent,
                                     "the keypoint frame holds fewer bytes than its own row count "
                                     "needs");
  }
  std::vector<Bearing> bearings(static_cast<size_t>(set.count));
  for (int32_t row = 0; row < set.count; ++row) {
    float xy[2] = {0, 0};
    std::memcpy(xy, rows.data() + static_cast<size_t>(row) * static_cast<size_t>(pitch),
                sizeof(xy));
    const Pixel pixel{static_cast<double>(xy[0]), static_cast<double>(xy[1])};
    const UnprojectedDirection unprojected = Unproject(lens, pixel);
    // Kept in place whether or not it is usable, so this vector stays parallel to the descriptor
    // rows the matcher will index it with. See `Bearing`.
    bearings[static_cast<size_t>(row)] =
        Bearing{unprojected.direction, pixel, unprojected.valid};
  }
  return Ok(std::move(bearings));
}

/**
 * What one descriptor element is, for the detector that wrote it.
 *
 * **Asked of the detector rather than guessed from the width**, which is what the first version did:
 * `width % 4 == 0 && width >= 512` reads a 512-byte row as 128 floats. A reviewer pointed out that
 * `AKAZE::DESCRIPTOR_KAZE` and `KAZE` write 256-byte `CV_32F` rows, which that heuristic calls
 * Hamming bytes — and that `ExtractFeatures` had the real `type()` in hand and threw it away.
 *
 * This engine only ever builds the three defaults, so keying off the detector is exact for anything
 * it produced. It is still a second copy of a fact: the type is decided where the descriptors are
 * written and re-derived here. The fix that removes the copy is a field on `FeatureSet`, which is a
 * contract change and is recorded rather than smuggled in beside a bug fix.
 */
int DescriptorType(FeatureDetector detector) {
  switch (detector) {
    case FeatureDetector::Sift:
      return CV_32F;
    case FeatureDetector::Orb:
    case FeatureDetector::Akaze:
    case FeatureDetector::Count:
      break;
  }
  return CV_8U;
}

/** The descriptor rows as a `cv::Mat` over the pinned bytes — a view, copied by nothing. */
Result<cv::Mat> ReadDescriptors(const FeatureSet& set, std::span<uint8_t> rows, int type) {
  if (set.count <= 0 || rows.empty()) {
    return Err<cv::Mat>(StatusCode::InvalidArgument, kComponent, "no descriptor rows to read");
  }
  // **The frame's own stride, for the reason `ReadBearings` takes the keypoint frame's.** This
  // divided the whole pinned span by the row count, which is the row width only when the pin hands
  // back exactly the rows the set claims — and `FeatureSet` is a value its caller fills in, so the
  // two can disagree. A set naming half the rows its frame holds made every descriptor twice as
  // wide, assembled from the bytes of two, and the only thing that noticed was the width comparison
  // against the other set. Falling back to the division is still right when a store leaves the
  // stride unset, which is what `MemoryFrameStoreAccess` did before it carried one.
  const int64_t pitch = set.descriptors.stride > 0
                            ? set.descriptors.stride
                            : static_cast<int64_t>(rows.size() / static_cast<size_t>(set.count));
  if (pitch <= 0) {
    return Err<cv::Mat>(StatusCode::InvalidArgument, kComponent,
                        "the descriptor frame holds fewer bytes than one row per feature");
  }
  // By division, so the product cannot wrap before the check that would have refused it — `size_t`
  // is 32 bits on wasm32, where `count * pitch` overflows at a plausible count and a wrapped
  // product would pass a comparison against the span it has already run past.
  if (static_cast<int64_t>(rows.size()) / pitch < set.count) {
    return Err<cv::Mat>(StatusCode::InvalidArgument, kComponent,
                        "the descriptor frame holds fewer bytes than its own row count needs");
  }
  // `CV_ELEM_SIZE` rather than a second table of our own: a `switch` here would be a copy of what
  // OpenCV already knows about its own type constants, and the two would drift the first time a
  // detector with a different element type arrived — which is the coupling `LumaRowBytesPerPixel`
  // exists in this file to remove, met again.
  const int64_t element = static_cast<int64_t>(CV_ELEM_SIZE(type));
  if (pitch % element != 0) {
    return Err<cv::Mat>(StatusCode::InvalidArgument, kComponent,
                        "the descriptor rows are not a whole number of elements wide for the "
                        "detector that wrote them");
  }
  // `cv::Mat` takes its dimensions as `int`. A row wider than that cannot be described to OpenCV,
  // and narrowing it would be undefined rather than wrong — so it is refused here, where the number
  // is still 64 bits wide.
  const int64_t columns = pitch / element;
  if (columns > std::numeric_limits<int>::max()) {
    return Err<cv::Mat>(StatusCode::InvalidArgument, kComponent,
                        "the descriptor rows are wider than OpenCV can describe");
  }
  return Ok(cv::Mat(set.count, static_cast<int>(columns), type, rows.data(),
                    static_cast<size_t>(pitch)));
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

  // A hypothesis further from the prior than the bound is not considered at all — not scored and
  // then out-voted, because the whole difficulty is that these *win* on inliers.
  const auto withinBound = [&](const cv::Matx33d& r) {
    return AngleBetween(FromMatrix(r), prior) * 180.0 / std::numbers::pi <= kPriorBoundDeg;
  };

  cv::Matx33d best = RotationMatrix(prior);
  std::vector<size_t> bestInliers;
  countInliers(best, &bestInliers);

  // **Deterministic *and* spread, which the first version was not.** It drew
  // `(7t+1, 13t+5, 23t+11) mod n`, so all 200 triples lay on one line in index space: about 200
  // distinct triples out of 4.4 million at 300 features, and at `n = 8` only four of the 56
  // possible. Theory wants ~34 *independent* triples for 99% confidence at a half-inlier ratio, and
  // correlated ones do not substitute — a reviewer perturbed the prior by one degree and ORB
  // refused three steps of eleven with "the best had 0", because no sampled triple was all-inlier.
  // The truth-shaped prior in the harness had been covering for it.
  //
  // A fixed-seed 64-bit LCG keeps the determinism the selection tests elsewhere in this core rely
  // on — same correspondences and same prior, same rotation — while drawing triples that are
  // independent of each other. The constants are Knuth's MMIX.
  uint64_t seed = 0x9E3779B97F4A7C15ULL;
  const auto next = [&seed](size_t bound) {
    seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
    return static_cast<size_t>((seed >> 33) % bound);
  };
  // Shrinks as the search succeeds: once a consensus of a given size is in hand, the draws needed
  // to be confident of having seen one that large are fewer, so a clean pair stops early and a
  // hard one is given the full budget the gate implies.
  int budget = RansacSampleBudget(0.0);
  std::vector<size_t> candidate;
  for (int iteration = 0; iteration < budget; ++iteration) {
    // **Drawn without replacement, so the confidence above is the confidence.** The first version
    // drew three independent indices and `continue`d on a collision — which spends a draw on
    // nothing. At `kMinimumCorrespondences = 8` the three are distinct only `8*7*6 / 8^3` of the
    // time, so 574 draws were 377 samples and the 0.99 that `kRansacConfidence` names was really
    // 0.9515. Shifting past the indices already taken costs two comparisons and makes every
    // iteration a sample.
    const size_t i = next(from.size());
    size_t j = next(from.size() - 1);
    if (j >= i) ++j;
    size_t k = next(from.size() - 2);
    if (k >= std::min(i, j)) ++k;
    if (k >= std::max(i, j)) ++k;
    cv::Matx33d sampled;
    if (!KabschRotation({from[i], from[j], from[k]}, {to[i], to[j], to[k]}, &sampled)) continue;
    if (!withinBound(sampled)) continue;
    countInliers(sampled, &candidate);
    if (candidate.size() > bestInliers.size()) {
      best = sampled;
      bestInliers = candidate;
      budget = std::min(budget, RansacSampleBudget(static_cast<double>(bestInliers.size()) /
                                             static_cast<double>(from.size())));
    }
  }

  if (bestInliers.size() < kMinimumCorrespondences) {
    // **`RegistrationFailed`, not `NotFound`.** The closed enum has carried this code since the
    // architecture was written and nothing had ever returned it; meanwhile both of this method's
    // registration failures returned `NotFound`, which `IFrameStoreAccess` uses for a handle naming
    // no frame. One code meaning "the pixels did not agree" and "that frame does not exist" is a
    // code a caller cannot branch on, which is the whole reason the enum is closed. It also let a
    // test assert the refusal by substring-matching `status.detail`, which the contract says is
    // never parsed.
    return Err<PairwiseResult>(StatusCode::RegistrationFailed, kComponent,
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
  if (KabschRotation(inFrom, inTo, &refined) && withinBound(refined)) {
    std::vector<size_t> after;
    countInliers(refined, &after);
    if (after.size() >= bestInliers.size()) {
      best = refined;
      bestInliers = after;
    }
  }

  // **Over the inliers, and bounded by `kInlierPx` by construction** — they are exactly the rows at
  // or under it. So this describes how *tightly* the accepted model fits its own support, which is
  // worth reporting, and it says nothing about whether that support is large enough, which is why
  // it is no longer part of `accepted` below.
  //
  // Taking it over *every* correspondence was the first attempt at fixing that and was worse: the
  // matcher leaves a majority of junk matches by design — measured at 181 correspondences to 60
  // inliers on a clean pair — so the all-correspondence median is an outlier's residual, around 300
  // pixels, and gating on it refused every step of a ring that registers perfectly well.
  std::vector<double> residuals;
  residuals.reserve(bestInliers.size());
  for (size_t at : bestInliers) residuals.push_back(residualPx(best, at));
  std::sort(residuals.begin(), residuals.end());
  const double median = residuals.empty() ? 0.0 : residuals[residuals.size() / 2];

  PairwiseResult answer{};
  answer.relativeRotation = FromMatrix(best);
  answer.inliers = static_cast<int32_t>(bestInliers.size());
  answer.correspondences = static_cast<int32_t>(from.size());
  answer.medianResidualPx = median;
  // **A fraction, because that is the conjunct that can be false.** A reviewer showed the previous
  // gate could not be: the count was guaranteed by the early return above, and the median was
  // guaranteed by being taken over the rows that gate selected. Injected noise from zero to four
  // pixels produced `accepted = true` at every level. What varies with the quality of the answer is
  // how much of the evidence stands behind it.
  const double agreeing =
      from.empty() ? 0.0 : static_cast<double>(bestInliers.size()) / static_cast<double>(from.size());
  answer.accepted = bestInliers.size() >= kMinimumCorrespondences && agreeing >= kInlierFraction;
  return Ok(answer);
}


}  // namespace

bool BearingsSpanAPlane(double largest, double second) {
  // **There is no zero guard, and that is deliberate.** One stood here saying it was what "lets the
  // ratio below be a division that cannot be by zero" — describing a division this body has not
  // contained since it became a comparison against `1e-9 * largest`. A reviewer then showed the
  // guard was unreachable as well as misdescribed: deleting it left every test green, including the
  // one that names it. `cv::SVD` orders the singular values, so `second <= largest`; an all-zero
  // covariance gives `0 > 0`, false, and a NaN gives false through the same comparison. Keeping an
  // untested branch because it feels safer is the thing this repository has a rule against.
  // **Relative, and deliberately still permissive.** This is the degeneracy that makes the fit
  // *unconstrained* — bearings all on one line, where the rotation about that line is free — and
  // not a general conditioning test. Bearings a ten-thousandth of a radian apart are badly
  // conditioned and this will pass them; what rejects those is that the rotation they produce wins
  // no inliers and loses the sample. Tightening this would be choosing a number with no measurement
  // behind it, and the search already has a gate that is measured.
  return second > 1e-9 * largest;
}

/**
 * How many triples to draw before giving up, for correspondences of which `agreeing` are inliers.
 *
 * **The budget is the acceptance gate turned into a number of draws, rather than a second knob.**
 * A triple drawn from a set in which a fraction `w` agree is all-inlier with probability `w^3`, so
 * `log(1 - p) / log(1 - w^3)` draws reach confidence `p` that at least one was. The first version
 * of this loop wrote 200 — the textbook figure for `w = 0.5` — and on a rendered ring the measured
 * ratios are a third of that, because a checkerboard panorama hands ORB hundreds of corners that
 * all look alike and the ratio test cannot separate them. At `w = 0.15` those 200 draws find an
 * all-inlier triple about half the time, and ORB refused three steps of eleven for exactly that
 * reason: `the best had 0`, `the best had 2`, and one consensus of 19 out of 141 that was fitted
 * well (0.82 px) and still under the gate. Nothing about the geometry was wrong; the search gave
 * up early and the harness could not tell the two apart.
 *
 * **Lowering `kInlierFraction` costs cubically, and that is worth knowing before anyone does it.**
 * The gate is this function's floor, so the budget is `log(1-p)/log(1-gate^3)`: at 0.2 it is 574
 * draws and a call takes about twenty milliseconds, and at 0.01 it is 4.6 *million* and the same
 * call takes fifteen seconds. Measured, by lowering the constant and watching a twenty-millisecond
 * test become a fifteen-second one. That is the honest arithmetic rather than a defect — accepting
 * a one-percent consensus means searching hard enough to find one — but the cost lives here while
 * the knob lives two hundred lines up, and the target device is a phone.
 *
 * A ratio below `kInlierFraction` is raised to it, and that is the whole of the clamping: a
 * consensus smaller than the gate would be *refused* even if it were found, so the draws that
 * would find one buy nothing. It also makes this total — `ratio >= 0.2` puts `w^3` in
 * `[0.008, 1)`, where the logarithm is finite and negative — which is what lets the loop hand it
 * the two-in-a-hundred-and-fifty it is currently sitting on without special-casing.
 */
int RansacSampleBudget(double agreeing) {
  const double ratio = agreeing > kInlierFraction ? agreeing : kInlierFraction;
  const double all = ratio * ratio * ratio;
  if (all >= 1.0) return 0;  // every correspondence agrees; there is nothing left to search for
  return static_cast<int>(std::ceil(std::log(1.0 - kRansacConfidence) / std::log(1.0 - all)));
}

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
      // The store's `Status` whole, component included — as `Extract` a few hundred lines up already
      // does. Rebuilding it with `kComponent` kept the code and the detail and overwrote the field
      // that says *who reported it*, so a caller diagnosing a failed pin was told this engine did
      // when the store did. Two methods of one class disagreeing about that is the second-copies
      // failure in miniature.
      if (!pinned->ok()) return pinned->status;
    }

    const Result<std::vector<Bearing>> bearingsA = ReadBearings(a, kaSpan.value, lens);
    if (!bearingsA.ok()) {
      return Err<PairwiseResult>(bearingsA.status.code, kComponent, bearingsA.status.detail);
    }
    const Result<std::vector<Bearing>> bearingsB = ReadBearings(b, kbSpan.value, lens);
    if (!bearingsB.ok()) {
      return Err<PairwiseResult>(bearingsB.status.code, kComponent, bearingsB.status.detail);
    }

    const int type = DescriptorType(detector_);
    const Result<cv::Mat> matA = ReadDescriptors(a, daSpan.value, type);
    if (!matA.ok()) return Err<PairwiseResult>(matA.status.code, kComponent, matA.status.detail);
    const Result<cv::Mat> matB = ReadDescriptors(b, dbSpan.value, type);
    if (!matB.ok()) return Err<PairwiseResult>(matB.status.code, kComponent, matB.status.detail);
    if (matA.value.type() != matB.value.type() || matA.value.cols != matB.value.cols) {
      return Err<PairwiseResult>(StatusCode::InvalidArgument, kComponent,
                                 "the two feature sets were not made by the same detector");
    }

    // **Lowe's ratio test, two nearest neighbours.** A single nearest neighbour always exists, so
    // matching without the ratio produces a full set of correspondences for two frames of unrelated
    // scenery — the shape that makes a registration look successful and be nonsense.
    const int norm = type == CV_8U ? cv::NORM_HAMMING : cv::NORM_L2;
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
      // A row the lens could not turn into a direction is dropped *here*, where dropping it costs
      // one correspondence, rather than in `ReadBearings`, where it shifted every index after it.
      if (!bearingsA.value[ia].usable || !bearingsB.value[ib].usable) continue;
      fromAll.push_back(bearingsA.value[ia].direction);
      toAll.push_back(bearingsB.value[ib].direction);
      observed.push_back(bearingsB.value[ib].pixel);
    }

    if (fromAll.size() < kMinimumCorrespondences) {
      // Registration failed for want of correspondences rather than for want of agreement among
      // them. Same code as the consensus failure above — both mean "these two frames did not
      // register" — and the detail says which, for a human rather than for a branch.
      return Err<PairwiseResult>(StatusCode::RegistrationFailed, kComponent,
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

#include "engines/frame_quality_engine/sharpness_frame_quality_engine.h"

#include <algorithm>
#include <cmath>

#include "utilities/pixel_format.h"

namespace sphanorama {
namespace {

constexpr const char* kComponent = "SharpnessFrameQualityEngine";

// The long edge the luma plane is reduced to before the Laplacian runs.
//
// Downscaling is not an optimisation here, or not only one. A Laplacian on a full-resolution
// frame responds to sensor noise as readily as to edges, and noise is what a *dark* frame has
// most of — so the unscaled measure would rank the noisiest frame of a burst highest, which is
// the opposite of the answer. Averaging into larger pixels first suppresses that and leaves the
// structure, and it makes the cost independent of what the camera happened to hand over.
constexpr int32_t kMeasureEdge = 256;

/**
 * How a format carries its luma, and the one place that is decided.
 *
 * `LumaAt` and `HasReadableLuma` below used to list the same five formats independently, and the
 * drift that allows is not theoretical: a reviewer deleted NV12 and I420 from `LumaAt`'s planar arm,
 * leaving them to answer `0.0`, and all 711 tests stayed green. `FeatureRegistrationEngine` collapsed
 * the same duplication a commit earlier; leaving it standing here is how "the fix went into one
 * engine and not the other" happens, which is a sentence this file's own comments already carry.
 */
enum class LumaKind { None, Planar, FourChannel };

LumaKind LumaKindOf(PixelFormat format) {
  switch (format) {
    case PixelFormat::Gray8:
    case PixelFormat::NV12:
    case PixelFormat::I420:
      return LumaKind::Planar;
    case PixelFormat::RGBA8:
    case PixelFormat::BGRA8:
      return LumaKind::FourChannel;
    default:
      return LumaKind::None;
  }
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

/** Luma at (x, y) for the formats a camera port can currently produce. */
double LumaAt(std::span<const uint8_t> bytes, PixelFormat format, int64_t stride, int64_t x,
              int64_t y) {
  // int64 arithmetic, then one conversion to the span's index type. A 4K frame's last row is
  // past 2^31 bytes in at four bytes a pixel, so the multiply has to be wide even though the
  // result always fits a size_t on the platforms this runs on.
  const int64_t row = y * stride;
  switch (LumaKindOf(format)) {
    case LumaKind::FourChannel: {
      const size_t at = static_cast<size_t>(row + x * 4);
      // Rec. 601 luma, in the frame's own channel order. Red and blue carry very different
      // weights — 0.299 against 0.114 — so reading BGRA as RGBA makes a blue frame look two and a
      // half times brighter than it is. Within one burst every frame comes from one camera in one
      // format, so it would not change a ranking today; it would the moment OfferFrame put an
      // imported frame in the same cell as a burst.
      const bool bgra = format == PixelFormat::BGRA8;
      const double red = bytes[at + (bgra ? 2 : 0)];
      const double blue = bytes[at + (bgra ? 0 : 2)];
      return 0.299 * red + 0.587 * bytes[at + 1] + 0.114 * blue;
    }
    case LumaKind::Planar:
      // The luma plane comes first in both planar layouts and is the whole of Gray8, so the
      // chroma that follows is simply never read.
      return bytes[static_cast<size_t>(row + x)];
    case LumaKind::None:
      // `Measure` refuses this format before pinning, so this arm is how the switch stays total
      // rather than a state anything reaches. It answers a number rather than a refusal, which is
      // why it must stay unreachable: a `0.0` here would be indistinguishable from a flat frame.
      break;
  }
  return 0.0;
}

bool HasReadableLuma(PixelFormat format) { return LumaKindOf(format) != LumaKind::None; }

}  // namespace

Result<SharpnessFrameQualityEngine::Measured> SharpnessFrameQualityEngine::Measure(
    const FrameRef& frame) {
  if (!HasReadableLuma(frame.format)) {
    return Err<Measured>(StatusCode::Unsupported, kComponent,
                         "this frame has no luma plane to read; an encoded frame needs decoding "
                         "before it can be scored");
  }
  if (frame.width <= 0 || frame.height <= 0) {
    return Err<Measured>(StatusCode::InvalidArgument, kComponent, "a frame with no pixels");
  }

  auto pinned = frames_.Pin(frame);
  if (!pinned.ok()) return pinned.status;

  // Undone on every path out, including the failures below: Pin promises the mapping until
  // Release, and an engine that kept one would pin a frame per burst until the session ended.
  struct Unpin {
    IFrameStoreAccess& frames;
    const FrameRef& frame;
    ~Unpin() { (void)frames.Release(frame); }
  } unpin{frames_, frame};

  // In int64 and narrowed once, because the obvious form is int32 * int32: a width near the top
  // of the range times four bytes a pixel overflows, and signed overflow is undefined behaviour
  // rather than a wrong number something downstream could sanity-check. The frame store had this
  // exact bug in its own stride arithmetic and it is worth being consistent about.
  //
  // One byte per pixel for the planar formats, which is what `LumaAt` reads of them: the luma
  // plane comes first and is all this engine touches. This used to say "`BytesPerPixel` answers 0
  // for those, hence the floor" and there is no floor any more — `LumaRowBytesPerPixel` answers
  // from the same `LumaKindOf` the read switches on, so the bound and the read cannot disagree
  // rather than merely happening not to. The registration engine's twin paragraph was updated when
  // that landed and this one was not, which is the shape this file's own comments warn about.
  const int64_t bytesPerPixel = LumaRowBytesPerPixel(frame.format);
  const int64_t rowBytes = static_cast<int64_t>(frame.width) * bytesPerPixel;
  const int64_t stride = frame.stride > 0 ? frame.stride : rowBytes;

  // **What the handle claims, against what the store actually handed over.** A `FrameRef` is a
  // plain value the caller passes in and `Find` keys on `id` alone, so nothing upstream makes its
  // geometry describe the allocation — the width, height and stride read above are the *caller's*
  // account of a frame, and `Pin` returns the entry's real span regardless.
  //
  // Without this, every read below indexed that span with that account. Reproduced under
  // AddressSanitizer by a reviewer: allocate 640x480 RGBA8 honestly, then score the same id
  // claiming `{4096, 4096, stride 16384}` — `heap-buffer-overflow READ 0 bytes after a
  // 1228800-byte region`. `wire::GetInteger` bounds the *cast* of those numbers where they cross
  // the boundary, which is a different question from whether they describe anything.
  //
  // A stride below the row's own width is refused separately, and not because it reads out of
  // bounds — it does not. Rows that overlap are not a frame anybody allocated, and what this would
  // score is a shear of the picture rather than the picture.
  //
  // `OfferFrame` is the door, and no shipped caller builds a handle by hand today. The callers it
  // was written for — a file import, a replay, a manual shutter — are exactly the ones that would,
  // and `Candidates()` publishes every frame id to the client.
  if (stride < rowBytes) {
    return Err<Measured>(StatusCode::InvalidArgument, kComponent,
                         "this frame's stride is narrower than one row of it");
  }
  // **Asked by division, because the multiply is the thing that overflows.** This guard widened
  // `width * bytesPerPixel` into int64 and then multiplied again without asking the same question
  // of the second product. With `stride <= 0` the fallback step is `rowBytes`, bounded by 2^33
  // rather than by `int32_t`, so `INT32_MAX` rows of it reaches ~1.8e19 and wraps **negative** — and
  // a negative `needed` is smaller than any size, so the guard passed the handle it exists to
  // refuse, and `LumaAt` then indexed a 16 KB span with a stride of 8,589,934,588.
  //
  // Found by a reviewer of `FeatureRegistrationEngine`, which was written from this function and
  // inherited the bug along with the guard. Widening one product and not its neighbour is the shape
  // to watch for; dividing never overflows.
  const int64_t held = static_cast<int64_t>(pinned.value.size());
  const int64_t claimedRows = static_cast<int64_t>(frame.height) - 1;
  if (claimedRows > 0) {
    // Exact, and the only check for a frame of more than one row: `stride <= (held - rowBytes) /
    // rows` is `rows * stride + rowBytes <= held` for positive `rows`, without forming the product.
    // A claim wider than the whole allocation makes the numerator negative, and every stride is
    // greater than a negative, so the same line refuses it.
    if (stride > (held - rowBytes) / claimedRows) {
      return Err<Measured>(StatusCode::InvalidArgument, kComponent,
                           "this frame claims more pixels than the store is holding for it");
    }
  } else if (rowBytes > held) {
    // One row, so there is no product to bound and the division above is skipped.
    //
    // **Unreachable here today, and kept anyway** — which is a position this codebase is otherwise
    // sceptical of, so the reason matters. `cols < 3 || rows < 3` below refuses every one-row frame
    // before any byte is read, so deleting this line changes nothing: over 347,875 adversarial
    // handles a reviewer measured the accepted set bit-identical with and without it. What it is
    // kept for is that the *same* line in `FeatureRegistrationEngine` is the sole bounds check for
    // that case — deleting it there is a `heap-buffer-overflow READ of size 400000` — and keeping
    // the two guards in step is the only thing that has stopped a fix landing in one and not the
    // other, three rounds running. Its deadness depends on an unrelated rule about grid sizes, and
    // the day that rule changes is the day this is load-bearing.
    //
    // "In step" rather than "identical", because they are not identical and cannot be: this one
    // says `claimedRows` where the other says `rows`, which is already taken further down. An
    // earlier draft claimed character-for-character sameness, which would make any `diff` between
    // the two read as a difference that mattered — and a check firing on a renamed local is a check
    // nobody keeps running.
    return Err<Measured>(StatusCode::InvalidArgument, kComponent,
                         "this frame claims more pixels than the store is holding for it");
  }

  // **The planar check this engine did not have, and needed more than the one it was ported from.**
  // For NV12 and I420 the buffer holds chroma after the luma plane, so the rows above bound the
  // bytes and not the picture: a handle claiming half again as many rows is in bounds, ASan stays
  // silent, and what comes back is a *wrong number* rather than a crash — on the value that decides
  // which frame of a burst survives. `OfferFrame` is `@facade`, so the caller's `FrameRef` arrives
  // here from the page unexamined.
  //
  // `FrameByteSize` counts packed bytes while the rows above are strided, so the stride has to be
  // pinned down too or a narrower claimed width buys budget for extra rows. Measured: this refuses
  // a handle that would otherwise score 26,940.9 on a frame whose picture scores 0.0.
  if (BytesPerPixel(frame.format) <= 0) {
    if (stride != rowBytes) {
      return Err<Measured>(StatusCode::InvalidArgument, kComponent,
                           "a planar frame's rows must be packed; this one claims a stride wider "
                           "than its width, which would hide chroma behind the claim");
    }
    const int64_t whole = FrameByteSize(frame.width, frame.height, frame.format);
    if (whole <= 0 || whole > held) {
      return Err<Measured>(StatusCode::InvalidArgument, kComponent,
                           "this frame claims more rows of picture than the store is holding for "
                           "it; the bytes past the luma plane are chroma");
    }
  }

  // Box-averaged into a grid no larger than kMeasureEdge on its long side. Integer block sizes
  // rather than interpolation: this is a noise filter that happens to shrink the image, and a
  // resampler would be a second thing to get wrong.
  const int64_t longest = std::max(frame.width, frame.height);
  const int64_t block = std::max<int64_t>(1, (longest + kMeasureEdge - 1) / kMeasureEdge);
  const int64_t cols = frame.width / block;
  const int64_t rows = frame.height / block;
  if (cols < 3 || rows < 3) {
    // A Laplacian needs a neighbour on every side. A frame this small is not a photograph, and
    // reporting a sharpness for it would be reporting the value of an empty sum.
    return Err<Measured>(StatusCode::InvalidArgument, kComponent,
                         "frame is too small to measure sharpness");
  }

  const size_t width = static_cast<size_t>(cols);
  std::vector<double> small(width * static_cast<size_t>(rows), 0.0);
  double total = 0.0;
  for (int64_t row = 0; row < rows; ++row) {
    for (int64_t col = 0; col < cols; ++col) {
      double sum = 0.0;
      for (int64_t dy = 0; dy < block; ++dy) {
        for (int64_t dx = 0; dx < block; ++dx) {
          sum += LumaAt(pinned.value, frame.format, stride, col * block + dx, row * block + dy);
        }
      }
      const double mean = sum / (static_cast<double>(block) * static_cast<double>(block));
      small[static_cast<size_t>(row) * width + static_cast<size_t>(col)] = mean;
      total += mean;
    }
  }

  // The four-neighbour Laplacian, and its variance over the interior. Variance rather than mean
  // magnitude because a smooth gradient — a wall, a sky — has a large first derivative and almost
  // no second one, so the mean would rank a photograph of a gradient above a photograph of a
  // texture. Edges are what change the response frame to frame; brightness is not.
  double sum = 0.0;
  double sumSquares = 0.0;
  int64_t count = 0;
  for (int64_t row = 1; row + 1 < rows; ++row) {
    for (int64_t col = 1; col + 1 < cols; ++col) {
      const size_t at = static_cast<size_t>(row) * width + static_cast<size_t>(col);
      const double response = small[at - width] + small[at + width] + small[at - 1] +
                              small[at + 1] - 4.0 * small[at];
      sum += response;
      sumSquares += response * response;
      ++count;
    }
  }

  Measured measured;
  const double mean = sum / static_cast<double>(count);
  // Clamped at zero: the algebraic form can go a hair negative on a perfectly flat frame through
  // floating-point cancellation alone, and a negative sharpness would sort below every real one
  // while meaning nothing.
  measured.sharpness = std::max(0.0, sumSquares / static_cast<double>(count) - mean * mean);
  measured.meanLuma = total / (static_cast<double>(cols) * static_cast<double>(rows));
  return Ok(measured);
}

Result<QualityScore> SharpnessFrameQualityEngine::Score(const FrameRef& frame, const PoseSample&,
                                                        const NodeContext& context) {
  auto measured = Measure(frame);
  if (!measured.ok()) return measured.status;

  QualityScore score;
  score.sharpness = measured.value.sharpness;

  // Agreement with the rest of this burst. Where the camera can hold an exposure lock across the
  // burst this should be near-perfect; it is measured anyway, because a lock is what a camera
  // *offers* rather than what it guarantees, and a device with no manual mode captures with the
  // exposure moving between frames. This is the number that says which happened.
  //
  // The siblings are re-measured rather than read off their stored scores, because QualityScore
  // has nowhere to carry a mean luma. That is n(n-1)/2 reads over a burst — ten for the default
  // five — and it is worth stating rather than hiding: if a burst ever grows past a handful of
  // frames, this wants the mean carried on the candidate instead.
  double agreement = 1.0;
  int32_t compared = 0;
  double drift = 0.0;
  for (const Candidate& sibling : context.siblings) {
    if (sibling.frame.id.value == frame.id.value) continue;
    auto other = Measure(sibling.frame);
    if (!other.ok()) continue;   // a sibling that cannot be read is not this frame's failure
    drift += std::abs(measured.value.meanLuma - other.value.meanLuma);
    ++compared;
  }
  if (compared > 0) {
    // Over the full 0–255 range, so a frame that agrees exactly scores 1 and one at the opposite
    // end of the range scores 0. Linear because the thing it feeds is a weighted sum, and a curve
    // here would be a second tuning knob hidden inside a measurement.
    agreement = std::max(0.0, 1.0 - (drift / compared) / 255.0);
  }
  score.exposureAgreement = agreement;

  // A per-frame summary, and **not** the number Rank sorts on. Rank normalises sharpness across
  // the candidate set before weighting it, and the set is the only place that range can be known —
  // so a value computed here from one frame cannot be on the same scale as one computed there.
  // The contract calls this "the single number selection sorts on", which was written before that
  // tension was visible; Rank takes the policy and Score does not, so Rank has to be the
  // authority and this is a readout beside it.
  //
  // The weights come from a default-constructed policy rather than being written out again, so
  // there is one place a default lives and this cannot drift away from it.
  const SelectionPolicy defaults;
  score.aggregate = defaults.weightSharpness * score.sharpness +
                    defaults.weightExposure * score.exposureAgreement;
  return Ok(score);
}

Result<std::vector<CandidateId>> SharpnessFrameQualityEngine::Rank(
    std::span<const Candidate> candidates, const SelectionPolicy& policy) {
  std::vector<CandidateId> ranked;
  ranked.reserve(candidates.size());
  for (const Candidate& candidate : candidates) ranked.push_back(candidate.id);
  if (candidates.empty()) return Ok(std::move(ranked));

  // Sharpness has no natural scale — it depends on contrast and on how much texture the scene
  // happens to contain — so weighting it against a [0,1] agreement would let the units decide the
  // answer instead of the policy. Normalising across the set is what makes the weights mean what
  // they say, and the set is the only place the range can be known.
  double sharpest = 0.0;
  for (const Candidate& candidate : candidates) {
    sharpest = std::max(sharpest, candidate.quality.sharpness);
  }

  const auto merit = [&](const Candidate& candidate) {
    const double sharpness = sharpest > 0.0 ? candidate.quality.sharpness / sharpest : 0.0;
    return policy.weightSharpness * sharpness +
           policy.weightExposure * candidate.quality.exposureAgreement;
  };

  std::vector<std::pair<double, uint64_t>> order;
  order.reserve(candidates.size());
  for (const Candidate& candidate : candidates) {
    order.emplace_back(merit(candidate), candidate.id.value);
  }
  // Best first, and equal merit falls back to the identity. Stable order matters more than which
  // of two equals wins: the build graph is keyed on the selection, so a ranking that reordered
  // them between runs would invalidate cached stages for nothing.
  std::sort(order.begin(), order.end(), [](const auto& a, const auto& b) {
    if (a.first != b.first) return a.first > b.first;
    return a.second < b.second;
  });

  ranked.clear();
  for (const auto& [score, id] : order) ranked.push_back(CandidateId{id});
  return Ok(std::move(ranked));
}

}  // namespace sphanorama

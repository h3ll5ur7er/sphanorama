#pragma once
#include <string>
#include <vector>

#include "sphanorama/resource_access/frame_store_access.h"
#include "sphanorama/types.h"

namespace sphanorama {

// One rendered frame and the rotation the generator rendered it at.
struct SyntheticFrame {
  // `RGBA8`, allocated in the store handed to the loader. **It belongs to the caller**, who must
  // `Forget` it — the same rule `IRegistrationEngine::ExtractFeatures` states for the frames it
  // hands back, and for the same reason: a harness that leaked a dataset per run would exhaust the
  // heap somewhere in the middle of a measurement and report that instead of an accuracy number.
  FrameRef frame;
  // Device to world, unit, exactly as `truth.json` spells it — including a negative scalar part,
  // which is the same rotation written the other way round. `ScoreRotations` handles the double
  // cover itself and nothing here should pre-empt it.
  Quat trueRotation;
};

// A dataset written by `tools/synth_dataset.py`, read back into a frame store.
//
// This is the join between the two halves of the accuracy harness. The generator renders the frames
// a phone would have captured and records where it was looking; `support/rotation_scoring` says how
// far an estimate is from that truth. Until this existed nothing in C++ read the first, so the
// median error Phase 2 exits on could not be computed at all.
//
// It deliberately does **not** re-implement the lens. ADR 0050 keeps the generator's projection
// independent of `utilities/camera_model` so that a shared error cannot cancel; this reads the
// recorded `intrinsics` and hands them over unexamined, which keeps that independence intact.
struct SyntheticDataset {
  // The lens every frame was rendered through. `rollingShutterLineTimeNs` and `estimated` are not
  // in the file and are left at the values that mean "not known" and "not an estimate" — these are
  // the true intrinsics, which is the whole point of a synthetic dataset.
  Intrinsics lens;
  std::vector<SyntheticFrame> frames;
};

/**
 * Read the dataset in `directory`, allocating one frame per entry in its `truth.json`.
 *
 * Per entry rather than per rendered image: a file in the directory that no entry names is not
 * read, and an entry naming a file that is not there is a refusal.
 *
 * Refuses with `NotFound` for a directory, a `truth.json` or a named frame file that is not there,
 * and `InvalidArgument` for a file that is there and is not what it claims — JSON that is not an
 * object, or whose `intrinsics` or a `frames` entry is not one; a `convention.rotation` that is not
 * the one this reader was written against; a header disagreeing with the recorded lens; a payload
 * shorter or longer than the header accounts for; a Netpbm that is not `P6`; a header field
 * missing, over-long, non-numeric or too large for the type that holds it; a maximum value this
 * reader cannot read; or an intrinsic that is not a finite number.
 *
 * **Codes from the store are forwarded, not translated.** `FrameStoreExhausted` from `Allocate`,
 * and whatever `Pin` or `Release` answered — so a store refusing `Release` with
 * `FailedPrecondition` refuses this call with `FailedPrecondition`. `Internal` is this reader's own
 * last resort and means one thing: an exception escaped the parse.
 *
 * **A refusal gives back every frame it allocated, and says so when it cannot.** That second half
 * is not pedantry. `Forget` is allowed to refuse — `MemoryFrameStoreAccess` returns a spill sink's
 * refusal and keeps the entry on its books — and the caller of a failed load holds no handles, so
 * a silent failure to roll back would leave bytes with nothing saying so. When every frame goes
 * back, which is the ordinary case, the store's totals are where they started and there is nothing
 * to clean up.
 *
 * **One recovery is not always available, and the difference is worth knowing.** Bytes left behind
 * by a refused `Forget` can be reclaimed with `Clear`. Bytes left behind by a refused *`Release`*
 * cannot: the frame stays pinned, and `Clear` refuses while any frame is pinned. A refusal that
 * mentions the pin is telling you the store needs rebuilding, not tidying.
 */
Result<SyntheticDataset> LoadSyntheticDataset(IFrameStoreAccess& store, const std::string& directory);

}  // namespace sphanorama

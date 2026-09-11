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
 * and `InvalidArgument` for a file that is there and is not what it claims — a header disagreeing
 * with the recorded lens, a payload shorter or longer than the header accounts for, a Netpbm that
 * is not `P6`, a first token too long to be a magic number, a maximum value this reader cannot
 * read, or an intrinsic that is not a finite number. `FrameStoreExhausted` and the codes `Pin` and
 * `Release` answer with come straight from the store. `Internal` is the last resort: an exception
 * escaping the parse, or a store that read a frame and then would not release the pin taken to
 * write it.
 *
 * **A refusal gives back every frame it allocated, and says so when it cannot.** That second half
 * is not pedantry. `Forget` is allowed to refuse — `MemoryFrameStoreAccess` returns a spill sink's
 * refusal and keeps the entry on its books — and the caller of a failed load holds no handles, so
 * a silent failure to roll back would leave bytes only `Clear` could recover with nothing saying
 * so. When every frame goes back, which is the ordinary case, the store's totals are where they
 * started and there is nothing to clean up.
 */
Result<SyntheticDataset> LoadSyntheticDataset(IFrameStoreAccess& store, const std::string& directory);

}  // namespace sphanorama

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
/**
 * **The bytes are signed, and nothing in the type system says so.**
 *
 * `truth.json`'s `convention.pixel_encoding` records that a frame's byte `b` means
 * `b / 255 * 2 - 1` — so 0 is -1.0, 128 is +0.00392, 255 is +1.0, with no gamma and no colour
 * space. The frames below are handed over as ordinary `RGBA8`, because that is what the store
 * allocates and there is no format that spells "signed unit range"; the loader copies bytes and
 * interprets none of them, so nothing here is wrong today.
 *
 * It is written here because the first thing to *compute* on these pixels will be wrong if it
 * assumes unsigned [0, 1] — every frame read with its contrast halved and its zero in the wrong
 * place, which is the kind of error that produces plausible numbers rather than a crash. A reviewer
 * pointed out that the loader validates one `convention` entry and drops the other five, of which
 * this is the one a consumer has to know.
 */
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
 * `FailedPrecondition` refuses this call with `FailedPrecondition`.
 *
 * `Internal` therefore has **two** sources and the code alone does not separate them: this reader
 * reports an exception that escaped the parse with it, and a store whose `Pin` answers `Internal`
 * has that forwarded. The `detail` distinguishes them and `component` does not — every refusal is
 * re-stamped as this reader's on the way out, which is worth knowing before anyone branches on it.
 * An earlier version of this paragraph said `Internal` "means one thing", which the branch's own
 * test disproves: `AwkwardStore::Pin` answers `Internal` and the load is asserted to answer it too.
 *
 * **A refusal gives back every frame it allocated, and says so when it cannot.** That second half
 * is not pedantry. `Forget` is allowed to refuse — `MemoryFrameStoreAccess` returns a spill sink's
 * refusal and keeps the entry on its books — and the caller of a failed load holds no handles, so
 * a silent failure to roll back would leave bytes with nothing saying so. When every frame goes
 * back, which is the ordinary case, the store's totals are where they started and there is nothing
 * to clean up.
 *
 * **What a refusal cannot tell you, so that nobody reads it as saying more.** Two things in
 * particular:
 *
 * The message is written from one rollback pass, and both destructors retry after the `Result`
 * exists — so a store that declines once and relents leaves nothing behind while the sentence
 * saying it declined has already been composed. The refusal reports what the store *said*, not what
 * the totals ended up being. Ask the store if you need the latter.
 *
 * There is a third exit, and on it nothing is said at all. If an allocation failure escapes the
 * function rather than being converted — which `-fno-exceptions` makes a terminate for every
 * consumer but this file's own tests — the rollbacks run from the two destructors, which have
 * nowhere to report to and discard the store's answer. So "says so when it cannot" covers the
 * refusal paths and not that one. It is bounded in life by the terminate, and it is stated here
 * rather than papered over.
 *
 * And there is no phrase that identifies the unrecoverable case. Bytes behind a refused `Release`
 * are the worst kind — the frame stays pinned and `Clear` refuses while anything is pinned, for the
 * life of the store — but the stride guard runs while pinned too and its refusal never mentions a
 * pin, so a reviewer showed "a refusal that mentions the pin" is no discriminator at all. Nor is
 * `Clear` a guaranteed remedy for the other case: `MemoryFrameStoreAccess::Forget` refuses by
 * forwarding a spill sink's `Drop`, and `Clear` forwards to that same sink's `Clear`, so the thing
 * that refused the first may refuse the second. Both claims were in an earlier version of this
 * paragraph and both were too strong.
 */
Result<SyntheticDataset> LoadSyntheticDataset(IFrameStoreAccess& store, const std::string& directory);

}  // namespace sphanorama

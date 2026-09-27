# 0068 — A preview is composed from the frames it is handed, each direction from the frame looking at it most squarely

**Status:** accepted

## Context

`ICompositionEngine` has been null since the architecture was written, and the first method worth
building is `RenderPreview`: `PanoramaBuildManager` shows a low-resolution preview first (roadmap,
Phase 2), and a preview is the first place a registration error becomes visible to anyone.

It could not be built as declared. `RenderPreview`, `FindSeams` and `BlendTile` read pixels, and
all three were handed a `GlobalSolution`, which names its frames by `FrameId`. The one route to a
frame's bytes is `IFrameStoreAccess::Pin`, and that takes a `FrameRef` — the handle with the
format, the shape and the stride. `CompensateExposure` already took its frames as a span beside the
solution; the other three had nowhere to get theirs from.

Two things a preview needs had no owner either. Nothing in the core turned a panorama pixel into a
direction, and the synthetic harness had frames and rotations but no picture of what a correct
composite should look like.

## Decision

1. **The composition methods that read pixels take the frames**, as `CompensateExposure` already
   did: `std::span<const FrameRef> frames`, where `frames[i]` is `solution.frames[i]`. A span that
   is not the solution's frames, in its order, is refused.

2. **`NearestCentreCompositionEngine::RenderPreview` is the first real composition.** Each pixel of
   a 2:1 equirectangular panorama is coloured from the frame whose optical axis is nearest its
   direction, among the frames whose image it falls inside; sampled bilinearly between pixel
   centres; multiplied by that frame's gain, if gains are given. Alpha is coverage: 255 where a
   frame sees the direction, and transparent black where none does, because black is a colour the
   scene produces. The answer is the largest even width no wider than `maxWidth`, allocated in the
   frame store, and it is the caller's to `Forget`. The other four methods answer `Unsupported`.

   Nearest centre rather than a blend, because a preview's job is to show the registration. Where
   it is right, two frames of one scene agree, and the choice does not show; where it is wrong, the
   error appears as a step on a line the frame centres predict, where a feather would smear it into
   something that reads as blur.

3. **`utilities/equirect` is the core's equirectangular convention**, the generator's, written
   once: longitude 0 is −Z and increases toward +X, latitude +90 is +Y at row 0, and an integer
   coordinate is a pixel edge. It is pinned to `tools/synth_dataset.py` by one hand-worked pixel,
   which both suites assert: a convention the two shared wrongly would round-trip perfectly
   (ADR 0050).

4. **The generator writes the answer key.** `--reference-width W` also writes `reference.ppm`: the
   photograph, sampled at a W-wide preview's pixel centres, in the frames' encoding.
   `LoadSyntheticReference` reads it. `composition_accuracy_test.cpp` renders a twelve-frame
   photograph ring, composes it with the rotations it was rendered at, and compares the covered
   pixels.

   Measured at 1024 wide, the photograph's own width, so the reference is the photograph's pixels
   unresampled:

   | | mean error, bytes | 99th percentile |
   | --- | --- | --- |
   | the true rotations | 1.193 | 11 |
   | alternate frames turned 0.1° either way | 2.84 | 29 |
   | the true rotations, frames sampled half a pixel off along their rows | 1.889 | 18 |
   | the true rotations, frames sampled half a pixel off in both axes | 2.448 | 23 |

   The bounds are 2.0 and 16. A tenth of a degree is under a third of a preview pixel and a fifth
   of the 0.5-degree threshold registration exits on, so a registration error the exit criterion
   allows is one this can see; a test asserts that it does. The gate and CI fail if the round trip
   skips or does not run.

## Consequences

- **Nearest centre is not the least blurry choice, and it was not chosen to be.** A variant that
  took the *last* frame that sees each direction measured 1.151, better than 1.193. A rectilinear
  frame samples the world more finely toward its edges than at its centre, so the frame looking
  most squarely at a direction is the one sampling it most coarsely. The preview is for seeing
  registration, and a sharper preview whose seams fall wherever the frame order puts them would be
  worse at that. Picking frames for quality belongs to `FindSeams` and `BlendTile`.
- **The comparison is not gauge-free.** A common rotation moves the whole preview against the
  reference, and the table's second row is as bad whether the turn alternates or is shared. With the
  true rotations there is no gauge to remove. An *estimated* solution has to be aligned to the truth
  first, which is what `rotation_scoring` already computes, and composing a registered ring is the
  next increment.
- **A gain multiplies the byte as an intensity**, which is what a camera frame's byte is. The
  synthetic frames' bytes are signed components (`b / 255 * 2 − 1`), and scaling one is not
  scaling the value it encodes. Interpolation is affine, so it is indifferent to the encoding. The
  harness passes no gains and nothing here claims to measure them.
- **The cost is every frame for every preview pixel**, pruned only by the nearest-so-far test:
  a render and its comparison take 1.4 to 1.5 seconds for 12 frames at 1024 wide in a debug build,
  where the single-pass engine this replaced took 1.1 to 1.2 — the price of the next bullet. A full
  sphere plans around sixty frames, and an index from direction to candidate frames is the fix when
  a measurement on a phone asks for it.
- **The memory is one frame and the answer**, not the sphere. A kept frame is about 4.9 MB against a
  128 MB ceiling on a mid-range phone (ADR 0023), so a sixty-frame sphere does not fit, and the
  first version, which pinned every frame at once, refused the preview of a real capture at any
  size. The preview is decided first, from geometry alone, as one frame index a pixel, kept in the
  preview's own red and green bytes with alpha 0 until the pixel is painted — so nothing that
  grows with the preview is held outside the store, only some sixty bytes a frame of bookkeeping.
  Then each frame that colours anything is pinned, painted from, released, and demoted back to the
  tier it was found in, as `CandidatePreview` does. A frame the store cannot put back is left in
  the heap and the refusal says so, and one it cannot say the tier of is not read at all — rather
  than a preview that quietly left either in the heap. The two-byte index is why a solution naming
  65,536 frames or more is refused.
- **A refusal can hand something back.** When the store will not take the preview back, the
  refusal's value is the preview — released if `ResidencyOf` says it is still pinned, then forgotten
  — because nothing else can name it to the store again. And a frame handed in whose release the
  store declines is left pinned, and out of its tier unless it was found pinned. The refusal's
  detail names the frame by its place in the solution and its id, and the tier to put it back in,
  but a detail is for a person and never parsed, so a program has to find the frame another way: it
  asks each frame's tier before the call and holds no pins of its own on them during it — a pin of
  the caller's own would read the same, since `ResidencyOf` cannot say whose a pin is. After a
  refusal, a frame whose tier has changed is out of place: released if it reads `HeapPinned`, as one
  the store would not release does, and then put back in the tier it had, which also covers one the
  store released but would not put back. Carrying the frame in the value instead was not done
  because the value is the preview's on the paths that hand it back, and one refusal can need both.
  This is the first `Result` in the core whose failure carries something to clean up, so the usual
  `if (!r.ok()) return r.status;` leaks the preview on those paths, and `PanoramaBuildManager`, when
  it calls this, has to look at the value. The alternative was to retry a declined release inside
  the engine until it took, which makes the refusal rarer and never removes it — and the first
  version's retry, run from a destructor, left frames half given back with a report that no longer
  described them.
- **A handle naming the id the preview is about to be given is refused, and it has to be asked
  after the answer is allocated.** Ids are never reissued within a store, but a new store counts
  from 1 again, so a handle from a session the store never adopted can name the id `Allocate` is
  about to give the preview — and it would then be read as the preview itself, painted from its own
  bytes, and answered `Ok`. Any other id the store does not hold is its `NotFound` when that
  frame's tier is asked, and is not asked at all of a frame that colours nothing.
- **No composition root selects it yet**, as with `Registration`: it is reached from tests, and
  `PanoramaBuildManager` still answers `Unsupported`.
- The dataset renderer (`Rendered`) moved from `registration_accuracy_test.cpp` into
  `core/test/support/rendered_dataset.h` so that both harnesses use it — unchanged in the commit
  that moved it, and then given the `referenceWidth` this harness asks for. `ReadFrame`
  takes the shape it expects and the sentence naming whose shape it is, so the reference is refused
  in its own words.

## Rejected alternative

**Carry `FrameRef`s in `GlobalSolution` instead of `FrameId`s.** It was the smaller edit to the
contract, and it would change what a solution is. `Refine` produces the solution and never reads a
pixel, so it would have to be handed handles it has no use for, only to pass them through. And a
solution crosses the boundary into the page, which would then hold the store's handles in a value
whose job is to say where frames point. The engine needs the handles at the moment it pins, and the
caller that holds them is the one to pass them in, as `CompensateExposure` already asked.

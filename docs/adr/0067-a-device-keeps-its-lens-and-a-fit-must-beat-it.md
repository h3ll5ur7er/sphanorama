# 0067 — A device keeps its lens, and a fit is taken only where it is surer than the lens it replaces

**Status:** accepted; amends [0066](0066-refine-fits-the-focal-length-from-the-ring.md) decision 3
by one condition

## Context

Every capture starts from a guess. No browser reports a camera's focal length, so the page assumes a
66-degree long edge (`ASSUMED_LONG_EDGE_FOV_DEG`) and derives the rest from the frame's shape; the
coverage plan is built on that, and `Refine` is handed it as `initial`. ADR 0066 made `Refine` fit
the focal length from a capture's loops, and measured why it matters: 2% out is Phase 2's whole
error budget, and the guess is not known to 2%.

But a phone's lens does not change between captures. A focal length fitted today is a better
starting point tomorrow than the guess, and several captures' fits together are better than any one
of them. Three things stand in the way of keeping it.

- **A fit judged only against the guess can make a kept lens worse.** ADR 0066 takes a fit wherever
  it is precise to 0.2%, and against a guess that is right: any precise fit beats a lens nobody
  measured. Against a lens already right it is not. Round 9 of ADR 0066's review measured a wide lens
  whose weak loops are fitted within that threshold, and against an *exactly right* lens left alone
  their rotations came out 8 to 13 times worse — 0.02 to 0.09 degrees against 0.002 to 0.011. ADR
  0066 recorded that as the next decision's to make, because only a lens that knows how sure it is
  can be weighed against a fit.
- **The two errors a fit reports combine differently across captures.** `focalSpread` is the pairs'
  noise, and a second capture averages it down; `focalModelError` is what a lens model a little
  wrong does to the least, the same lens misread the same way every time, and a second capture
  leaves it where it was (ADR 0066 decision 4). A kept lens that pooled them would grow surer of an
  error that is not shrinking.
- **Nothing writes a fitted lens yet.** `PanoramaBuildManager` answers `Unsupported`, and no
  composition root selects the OpenCV registration engine, so `Refine` is reached from tests alone.
  Where a kept lens is stored and which manager writes it can be decided now, and built only with
  that writer.

## Decision

1. **A lens says how sure it is.** `Intrinsics::focalUncertainty` is the standard deviation of the
   focal length's natural log — about the fraction it could be out. Infinity, the default, is a
   guess, which is what `LensFromFieldOfView` and the page's assumption answer; zero is a lens known
   exactly. A lens `Refine` fits carries its fit's `focalSpread` and `focalModelError` combined, as
   ADR 0066 combines them for its threshold. `IsUsableLens` does not read it: it says how well a lens
   is known, not where a direction lands.

2. **`Refine` takes a fit only where it is surer than the lens it was handed.** ADR 0066's conditions
   stand, and one is added: the fit's combined uncertainty must be less than `initial.focalUncertainty`.
   Against a guess every precise fit is surer, so nothing changes for a lens read from a field of
   view; against a kept lens, a weak loop's fit no longer replaces a better one. Where the fit is not
   taken its `focalSpread` and `focalModelError` are still reported, so a caller can see what it
   would have been. A `focalUncertainty` that is not a figure — NaN, which compares false against
   everything and would let every fit through, or below zero — is refused as `InvalidArgument`.

3. **What a device keeps, and how a capture amends it.** `utilities/kept_lens` holds `KeptLens` —
   the lens, its noise and its model error kept apart, and how many captures it was made from — and
   `AmendKeptLens`, the rule for one more capture:
   - Only a capture `Refine` fitted amends it, and only its focal length. A refused capture's least
     was not precise, or not surer than the lens it was handed, and its rotations were not solved
     under it; distortion and the principal point are not fitted (ADR 0066).
   - Captures are weighed by their own noise, in the natural log of the focal length. The kept noise
     is the inverse-variance combination and shrinks as captures agree; the kept model error is the
     same weighting of each capture's own, because that is the bias of the weighted mean, and it does
     not shrink. `lens.focalUncertainty` is the two combined, which is what the next `Refine` weighs
     a fit against.
   - The focal length is read as a fraction of the long edge, so a capture at another frame size
     amends the same lens.
   - The first fitted capture is taken whole. A lens known exactly is not moved, and a capture known
     exactly replaces a lens that is not, because an infinite weight is not something to average.

   A utility rather than an engine because it is one formula with no alternatives on the table, and
   rather than a manager's private code because two managers will need it and managers do not call
   managers.

4. **Where it is kept, and who reads and writes it** — decided here, built with its writer:
   - **Keyed by camera and mode**: the track's `deviceId` and the frame size it settled on. A
     `deviceId` is per origin and resets when site data is cleared, which is exactly the lifetime of
     the store it is kept in, so a key and its lens are lost together or not at all.
   - **Stored as a device document in V12's store.** `IProjectStoreAccess` already persists documents
     to IndexedDB behind a resident copy (ADR 0014); a device-scoped document there is the same
     volatility — where metadata is persisted — with a different key, not a new resource access.
     Normalised by the long edge on the way in.
   - **Read by `CaptureSessionManager`** at `Begin`, to plan from the kept lens's field of view where
     one exists rather than the 66-degree assumption, and **written by `PanoramaBuildManager`** after
     a build: the kept lens handed to `Refine` as `initial`, and `AmendKeptLens` applied to its answer.

5. **A new axis.** "What this device's camera is known to be" varies with the device, the browser's
   camera identity, and how estimates are combined, and nothing in the volatility map owned it. It is
   V17: `utilities/kept_lens` for how a capture amends it, V12's store for where it lives.

## Consequences

- **A contract change** in `types.h` (`Intrinsics::focalUncertainty`, and one more condition on
  `GlobalSolution::lensFitted`) and in `engines/registration_engine.h` (`Refine`'s lens paragraph,
  and a refusal for an uncertainty that is not a figure). The TypeScript mirror moves with them. A
  lens built anywhere without the field set is a guess, so every existing caller behaves as before.
- **A utility with no caller in `core/src` yet.** `kept_lens` is reached from its tests alone until
  the build manager runs `Refine`, the cost ADR 0062 took for `rotation_averaging` and for the same
  reason: the rule can be decided and tested before its caller exists, and building the storage now
  would be storage nothing writes.
- **A kept lens that is wrong and sure stays wrong.** Only a surer fit displaces it, and the model
  error it carries is a scale rather than a bound (ADR 0066's consequences: a k1 misreading, radial
  only). A capture whose fit disagrees with the kept lens by many of their combined deviations is the
  signal that it is wrong, and nothing yet reads it; clearing site data is today's only reset.
- **The wide-lens cost ADR 0066 recorded closes once a lens is kept, not before.** The first capture
  on a device is still weighed against the guess, and weak loops on a wide lens are still fitted then.
- **Captures as good as the first do not improve the kept lens.** A strong shape's uncertainty is
  mostly its model error — a twelve-frame ring reads 0.014% of noise and 0.046% of model error — and
  the next capture of the same shape shares it, so its fit is no surer than the lens it would
  replace and is not taken, and only a taken fit amends the lens. The kept lens improves when a
  capture is surer than it: a stronger shape, or less noise. Averaging in every capture's noise
  would shrink a figure the model error keeps large, which is the rejected alternative below.
- **One camera mode's lens is not used for another.** A phone that settles on a different frame size
  next time starts from the guess again, rather than risk a crop the long edge does not describe.

## Rejected alternatives

**Combine the fit with the lens handed in, inside `Refine`.** The better estimate of one capture is
the inverse-variance mean of its fit and the kept lens, solved once more at that scale. Rejected
because the kept lens would then be counted twice: once in `Refine`'s answer and again when the
caller amends the kept lens with it. Keeping `Refine` to "the better of the two" and the combination
in `AmendKeptLens` counts every capture once.

**Amend the kept lens with every least, weighed by its noise.** A capture `Refine` refused still
measured something, and inverse-variance weighting would give it the small weight it deserves.
Rejected because the weight is the noise and the danger is the model error: a weak loop reads 0.3%
of model error with a noise that can be under 0.15% (ADR 0066), so its least would be weighted as
though it were good. Only fits `Refine` took, which passed both, amend the lens.

**A new resource access for device facts.** Where a document is persisted is V12's volatility
whatever the document is about, and a second port over the same IndexedDB would be two owners of one
axis.

**Keep the lens on the page, in `localStorage`.** The page would then own a fact the core computes
and consumes; the core is where a fit is made and where it is read, and the page's job is to persist
what it is handed.

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
   taken its `focalSpread` and `focalModelError` are still reported, and where it was precise —
   every condition but this one — so is where it lies: `GlobalSolution::focalScale`, as a multiple
   of the lens returned, one where the fit was taken and zero where there was no precise least. A
   `focalUncertainty` that is not a figure — NaN, which compares false against everything and would
   let every fit through, or below zero — is refused as `InvalidArgument`.

3. **What a device keeps, and how a capture amends it.** `utilities/kept_lens` holds `KeptLens` —
   the lens, its noise and its model error kept apart, and how many captures it was made from —
   `AmendKeptLens`, the rule for one more capture, and `KeptLensFor`, the lens a capture is handed
   (decision 4):
   - **Every precise least amends it, taken or not**, and only in its focal length. The first
     version amended only with fits `Refine` took, and froze: a second capture of the same shape
     shares the first's model error, so its fit is surer than the lens kept from the captures before
     it only where its noise is below that lens's, and in the rings measured nothing was taken from the third capture on — 2 of 20
     rings amended the lens (round 1). Whether
     `Refine` takes a fit is a question about that capture's rotations; whether a least measured
     the lens is ADR 0066's precision, and a capture answered under the kept lens still measured it.
     A least that is not precise amends nothing: taken whole as a first measurement, a flat cost's
     least tens of percent out would be the lens the next capture is planned from. Distortion and
     the principal point are not fitted (ADR 0066).
   - **Weighed for the least uncertainty.** The model errors add rather than combining as
     independent errors — both are the one lens misread, and each is reported as a size without
     its sign, so they are taken to lean the same way — and the weight between the kept lens and
     the capture is the one that leaves `hypot(noise, modelError)` least, in closed form and clamped
     to the two of them. Where the model errors agree, as they do for a device capturing the plan
     its lens gives it, that is the inverse-variance weight by the noise, the noise shrinks as
     captures agree and the model error stays. Where they differ it is never less sure than either
     input, which the noise alone did not promise: a quiet lens with a large model error outweighed
     a capture `Refine` had just found surer, and left the kept lens at 0.142% from a capture at
     0.048% (round 1).
   - **Exact is both figures zero**, and it falls out of the weight rather than being a case: a lens
     known exactly is not moved, and a capture known exactly replaces a lens that is not. The noise
     alone at zero is not exact, and keying on it froze a lens with a model error.
   - The focal length is read as a fraction of the long edge, so a capture at another size of the
     same shape amends the same lens. A frame of another shape is refused: it is a crop, which the
     long edge does not describe.
   - The first measurement is taken whole. A kept lens that cannot project, figures that are not
     finite and at least zero or that combine past the largest double, a negative count, and a
     capture that would leave a kept lens that cannot project are refused, since a lens made from
     one would be one every later call refuses — and so could never be replaced (round 3).

   A utility rather than an engine because it is one formula with no alternatives on the table, and
   rather than a manager's private code because two managers will need it and managers do not call
   managers.

4. **Where it is kept, and who reads and writes it** — decided here, built with its writer:
   - **Keyed by camera and frame shape**: the track's `deviceId` and the shape of the frame it
     settled on, reduced — 4:3, not 1280 x 960 — since `AmendKeptLens` reads another size of one
     shape as the same lens and refuses another shape. A `deviceId` is per origin and resets when
     site data is cleared, which is exactly the lifetime of the store it is kept in, so a key and
     its lens are lost together or not at all.
   - **Stored as a device document in V12's store.** `IProjectStoreAccess` already persists documents
     to IndexedDB behind a resident copy (ADR 0014); a device-scoped document there is the same
     volatility — where metadata is persisted — with a different key, not a new resource access.
     Stored at the size it was kept at, which is how `AmendKeptLens` answers it: the lens, its noise,
     its model error and its count, and not `focalUncertainty` or `estimated`, which the figures and
     the count imply and are a second copy of them — read back unset the uncertainty is a guess,
     which any fit replaces, and zeroed it is exact, which none does (round 2). A guess is stored as no document at all: a kept lens's
     figures are finite and so is their combination, which `AmendKeptLens` refuses otherwise, so
     nothing written needs an infinity the document format may not spell. A document that does not read back whole is
     refused and the capture starts from the guess, never read with its missing figures as zero —
     zero is a lens known exactly, the one reading that nothing afterwards would ever move. So is a
     document that reads back whole and is not a kept lens, or is one of another shape than the key
     it was read under — `KeptLensFor` answers `FailedPrecondition` for these, and for nothing else:
     `PanoramaBuildManager` deletes it, the capture starts from the guess, and its own least, if it
     has a precise one, is taken whole and written in its place, rather than a key every later build
     is refused on (rounds 3 and 5). A refusal of the call — `InvalidArgument`, a frame of no size or
     one this lens cannot project at — leaves the document be, and that capture starts from the
     guess (round 4). `CaptureSessionManager` plans from the guess on any refusal and writes
     nothing, so the document has one writer (round 5).
   - **Read by `CaptureSessionManager`** at `Begin`, to plan from the kept lens's field of view where
     one exists rather than the 66-degree assumption, and **written by `PanoramaBuildManager`** after
     a build: `KeptLensFor` handed to `Refine` as `initial`, and `AmendKeptLens` applied to its answer.
     `KeptLensFor` is the kept lens at the capture's frame size — its lengths in pixels scaled by the
     long edge, since a capture's matches are pixels of the frame it was grabbed at and `Refine`
     reads them through the lens it is handed — with `focalUncertainty` derived from the figures
     kept. It answers `NotFound` where nothing is kept, and the caller starts from its guess. The
     same lens goes to `EstimatePairwise`: where `Refine` does not take a fit it answers with the
     pairs' own rotations, so a capture answered under the kept lens is only as good as the lens
     its pairs were estimated under — a median of 0.97 degrees on a ring whose pairs came from
     another, where the fit taken gives 0.037 (round 2). Refitting at the kept lens on that one
     refusal would fix it there and not on ADR 0066's, which pass the pairs' rotations through too.

5. **A new axis.** "What this device's camera is known to be" varies with the device, the browser's
   camera identity, and how estimates are combined, and nothing in the volatility map owned it. It is
   V17: `utilities/kept_lens` for how a capture amends it and how it is handed to one, V12's store
   for where it lives.

## Consequences

- **A contract change** in `types.h` (`Intrinsics::focalUncertainty`, `GlobalSolution::focalScale`,
  and one more condition on `GlobalSolution::lensFitted`) and in `engines/registration_engine.h` (`Refine`'s lens paragraph,
  and a refusal for an uncertainty that is not a figure). The TypeScript mirror moves with them. A
  lens built anywhere without the field set is a guess, so every existing caller behaves as before.
- **A utility with no caller in `core/src` yet.** `kept_lens` is reached from its tests alone until
  the build manager runs `Refine`, the cost ADR 0062 took for `rotation_averaging` and for the same
  reason: the rule can be decided and tested before its caller exists, and building the storage now
  would be storage nothing writes.
- **A kept lens that is wrong and sure is slow to leave it.** Each capture moves it only by the
  weight its figures earn, and the model error it carries is a scale rather than a bound (ADR
  0066's consequences: a k1 misreading, radial only). A capture whose least lies many of their
  combined deviations from the kept lens is the signal that it is wrong: `intrinsics.fx` times
  `focalScale` against the `fx` handed in, since `focalScale` alone is one wherever the fit was
  taken, which is where the capture was surer than the kept lens (round 2). Nothing yet reads it;
  clearing site data is today's only reset.
- **The wide-lens cost ADR 0066 recorded closes once a lens is kept, not before.** The first capture
  on a device is still weighed against the guess, and weak loops on a wide lens are still fitted then.
- **The kept lens stops improving at its model error.** A strong shape's uncertainty is mostly its
  model error — a twelve-frame ring reads 0.014% of noise and 0.046% of model error — and more
  captures of the same shape shrink only the noise, so the combined figure approaches 0.046% and
  stays there. `Refine` answers under the kept lens wherever a capture's own figure is not below it
  — for a capture of the same shape, wherever its noise is not below the kept lens's — and each
  capture's measurement amends the lens either way.
- **The order captures arrive in matters where their model errors differ.** Each amend is the
  best combination of the two it is handed, not of every capture there has been; where the model
  errors agree the two are the same and order does not matter. The rejected alternative below is
  what order independence would cost.
- **Another shape of frame starts from the guess, and another size of one shape does not.** Reading
  a size as the same lens assumes the camera scales its whole sensor to it. A mode that crops to
  the same shape — a digital zoom — would be averaged in as the same lens, and nothing here can
  tell it apart.

## Rejected alternatives

**Combine the fit with the lens handed in, inside `Refine`.** The better estimate of one capture is
the inverse-variance mean of its fit and the kept lens, solved once more at that scale. Rejected
because the kept lens would then be counted twice: once in `Refine`'s answer and again when the
caller amends the kept lens with it. Keeping `Refine` to "the better of the two" and the combination
in `AmendKeptLens` counts every capture once.

**Amend the kept lens only with fits `Refine` took.** The first version of this ADR, on the
argument that a refused fit's rotations were not solved under it. Rejected in round 1 because it
froze the lens after two captures, above: a fit is refused for being no surer than the kept lens,
which says nothing against it as a measurement.

**Amend it with every least, precise or not, weighed for its uncertainty.** The weight would give a
weak loop the small say its figures deserve. Rejected because a first measurement is taken whole,
and because the model error is a scale rather than a bound (ADR 0066): a weak loop's figures are
the least trustworthy ones there are, and ADR 0066's precision is the line below which they are
not believed.

**Weigh each capture by its noise alone.** The textbook inverse-variance mean, and the first
version of this ADR. Rejected because the noise is not the whole of the uncertainty: weighed by it,
a quiet lens with a large model error outweighs a capture with a small one, and the answer can be
less sure than the capture was.

**Keep every capture and weigh them all at once.** Order independence where the model errors
differ, at the cost of a document that grows with every capture. Rejected because a device captures
the plan its lens gives it, whose model errors agree, and there the pairwise rule is already the
whole answer.

**A new resource access for device facts.** Where a document is persisted is V12's volatility
whatever the document is about, and a second port over the same IndexedDB would be two owners of one
axis.

**Keep the lens on the page, in `localStorage`.** The page would then own a fact the core computes
and consumes; the core is where a fit is made and where it is read, and the page's job is to persist
what it is handed.

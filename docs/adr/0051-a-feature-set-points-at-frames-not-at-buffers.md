# 0051 — A feature set points at frames, not at buffers

**Status:** accepted

## Context

`IRegistrationEngine::ExtractFeatures` returns a `FeatureSet` carrying `BufferId descriptors` and
`BufferId keypoints`, commented "opaque, lives in the frame store". Written before anything
implemented it, and the first attempt to implement it found the store cannot serve that shape.

`IFrameStoreAccess` allocates **frames** — `Allocate(width, height, PixelFormat)` — and pins them
as `span<uint8_t>`. There is no arbitrary-buffer API and no way to obtain or resolve a bare
`BufferId`. The only `BufferId` a caller ever holds is the one inside a `FrameRef`, which the store
issued as part of a frame.

So `FeatureSet` names a resource nothing can produce.

## Decision

**`FeatureSet` carries `FrameRef descriptors` and `FrameRef keypoints`.**

Descriptors are a `Gray8` allocation of `count` rows by the detector's descriptor width; keypoints
likewise. They are allocated, pinned, released, spilled and budgeted by exactly the machinery
frames already use.

- **It reuses tested machinery rather than adding untested surface.** The spill tier, the residency
  ladder, `Budget()`, `ContentHash`, the tier generation — all of it applies unchanged. A separate
  buffer API would need every one of those decisions made again.
- **It changes an engine-internal type, not a resource access.** `IFrameStoreAccess` is implemented
  twice, in browser TypeScript and natively, behind a shared contract suite. Widening it means
  growing both and adding cases to that suite. `FeatureSet` is produced and consumed inside the
  engine layer.
- **The budget is the point, not an accident.** Descriptors are not small: sixty frames of ORB at
  ~500 features is roughly 1 MB, and SIFT's 128 floats per feature is sixteen times that — 512 bytes
  a row against 32, so 15.36 MB. An earlier draft said "fifteen times", which was the megabytes read
  as a multiplier.

  That arithmetic also assumed 500 features for *every* detector, and a reviewer showed the code did
  not: ORB caps itself at 500 by default, while `cv::SIFT::create()` and `cv::AKAZE::create()` are
  unbounded, returning 1,328 and 2,547 on this repository's test texture at 768 square. The engine
  now caps all three at one shared budget, which is what makes the paragraph above true rather than
  aspirational — and asking for a cap turned out not to be getting one, for two different reasons
  this ADR first gave as one. `retainBest` keeps everyone tied with the last of its selection, which
  is why SIFT answers 501 to a request of 500 on that texture. ORB overruns by another mechanism
  entirely: `orb.cpp` applies the cap per pyramid level and concatenates the levels, so its total is
  bounded nowhere. A frame ruled into eight-pixel squares at 768 square shows the difference at
  scale — asked for 500, ORB returns 1,145 and SIFT 740, while AKAZE returns exactly 500. The engine
  therefore truncates as well as asks. Under
  memory pressure the store must be able to evict them, and only a store-managed allocation can be.

**The rows come back best-first, always, and that is a promise to the caller rather than an
implementation detail.** It is recorded here, in the ADR that shaped `FeatureSet`, because
`FeatureSet`'s own comment now states it and matching will be built on it — a contract-visible
behaviour that went into the code with no decision written down anywhere.

Three detectors, three native orders: ORB's is level-major, SIFT's is neither sorted nor
response-major, and AKAZE's is sorted only when its cap binds. Keeping the first `count` rows of
that would discard better features than it kept — for ORB, whole octaves of them. So the engine
sorts by response and then truncates, which makes the cap keep the best rather than the first and
gives every detector one order a caller can rely on instead of three a caller cannot tell apart.

**The frames a `FeatureSet` carries *into* a call belong to the caller too.** `EstimatePairwise`
takes two of them, so four store allocations, and may pin and release their frames but must not
`Forget` any — on a refusal either. A reviewer pointed out that this ADR wrote an ownership rule for
the frames coming *out* of `ExtractFeatures` and left the inbound direction unstated, which was
unstatable before this change (they were `BufferId`s naming nothing) and is live now. It matters
because `Refine` reuses each `FeatureSet` across several pairs, so an implementation that tidied up
after itself would pull the bytes out from under every later pair naming the same frame — and the
store answers the *second* `Forget` with `NotFound`, so the undetectable one is the first.

The sort is stable, and the reason is the tie order rather than repeatability. `std::sort` would be
exactly as repeatable; what it would not preserve is the detector's own order among equal responses,
which is the half of the promise a caller has no way to predict for itself.

Two costs are accepted rather than hidden:

- **"Best" is not "representative".** The order is by the detector's own response, and ORB's is a
  per-level Harris score with no normalisation between levels, so the strongest rows cluster at one
  scale — 48 of the top 50 on this repository's texture at 768 square, out of a set spanning eight
  octaves. A matcher wanting features spread across scales has to arrange that itself, and
  `FeatureSet` says so rather than leaving it to be discovered during Phase 2.
- **The response is not in the output, so the caller cannot check the order it is promised.** That
  is deliberate: the score's scale is detector-specific and not comparable between frames, so a
  caller thresholding on it would be reading a different quantity for each detector. The cost is
  that best-first rests on this engine's tests rather than on anything the caller can verify.

**`Gray8` names the container, not the contents, and that is the ordinary way this is done.** In
OpenCV a descriptor block is a `cv::Mat` — the same type that holds images — and nobody reads that
as a claim that descriptors are pixels. `Mat` is a rectangular block of same-sized elements, and so
is a frame here. What matters is that the container supports the intended use: `count` rows of a
fixed width, tightly packed with `stride == width`, allocated and evicted by the store.

So this is a naming convention rather than a misdescription, and `PixelFormat::Opaque` would be a
wider change buying a label.

## Consequences

- `contracts/ts/contracts.d.ts` regenerates: `FeatureSet`'s two fields change type. Nothing in the
  shell reads it — no interface carrying it is marked `@boundary` — so the mirror moves and no
  TypeScript does.
- **A `FrameRef`'s `format` now names a container rather than describing contents**, in this one
  place. SIFT's 128 floats are 512 bytes a row; ORB's are 32; AKAZE's 61. A consumer knows the
  layout because it knows the detector, exactly as an OpenCV caller knows what is in a `Mat`. Worth
  writing down because it is the first `FrameRef` in the repository that is not an image, and a
  reader meeting one should find the convention stated rather than infer a mistake.
- The engine now owns frame lifetimes it did not before: whoever calls `ExtractFeatures` owns two
  more `FrameRef`s and must `Forget` them. That is a new leak surface, and the contract header has
  to say who owns them.

  An earlier draft of this ADR added "which it currently does not for anything", and a reviewer
  showed that was false when it was written: `ICameraAccess::PeekPreviewFrame` says it in nearly the
  same words and has a contract-suite assertion behind it. The rule this follows is established, not
  new.

  The correction to that correction, from the next round, is the part worth keeping. The first fix
  also named `IImageCodecAccess::Decode` and `ICompositionEngine::BlendTile` as further precedents,
  and they are not: they hand back frames the same way and say **nothing** about ownership.
  `ICompositionEngine::RenderPreview`, `IPanoramaBuildManager::Panorama`,
  `ICaptureSessionManager::Candidates` and `IFrameStoreAccess::Allocate` itself are more. So there is
  exactly one documented precedent and a handful of undocumented surfaces beside it — a gap this ADR
  does not close, since widening it to four more contracts would be scope this change did not ask
  for. The count is deliberately not given: four attempts at one were each short by one, and the
  argument never rested on it.

  Some of them are not leaks at all, which is the part worth carrying: a `Panorama`'s tiles and a
  cell's candidates are frames the core still holds, so a caller reading this ADR's rule onto them
  would forget a frame still in use. A *second* `Forget` answers `NotFound`; the first is the one
  nothing catches. Both corrections are left here rather than tidied away: an overclaim repaired with a smaller
  overclaim is the failure this repository keeps making, and it is only legible if the sequence
  survives.

**One instance of the defect this ADR is named for is still in the contracts.** `SeamMap` carries
`BufferId labelBuffer` (`types.h`), which names exactly the resource this ADR established nothing can
produce or resolve — the frame store allocates frames and there is no `AllocateBuffer`. It is not
reached today because `ICompositionEngine` is null, but it is the return type of the engine Phase 2
builds next, so the next person to implement seam finding meets the same dead end `FeatureSet` met.
It is left rather than fixed here because changing it is a contract change for an engine this PR does
not touch, and bundling it would make this ADR about two things. Recorded so it is inherited as a
known gap rather than rediscovered — the same treatment ADR 0052 gives the geometry-versus-byte-count
seam.

## Rejected

***Adding `AllocateBuffer`/`PinBuffer` to `IFrameStoreAccess`.*** The most faithful reading of the
original comment, and the heaviest: two implementations to grow, a shared contract suite to extend,
and every residency and budget question answered a second time for a second kind of thing.

***Handing back the detector's own order and letting each caller sort.*** Moves one engine's work
to every caller, and cannot be done at all without putting the response in the output — the field
deliberately left out just above. It would also make the cap dishonest, since the truncation happens
before any caller sees the list: sorting afterwards would order rows that had already been chosen
for the wrong reason.

***Returning descriptor bytes by value in `FeatureSet`.*** Simplest to write, and legitimate since
engines never cross the boundary. Rejected on the budget: sixty frames of descriptors resident with
nothing able to evict them under pressure, in a core whose frame store exists precisely because a
phone runs out of memory.

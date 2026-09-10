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
  ~500 features is roughly 1 MB, and SIFT's 128 floats per feature is fifteen times that. Under
  memory pressure the store must be able to evict them, and only a store-managed allocation can be.

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
  to say who owns them, which it currently does not for anything.

## Rejected

***Adding `AllocateBuffer`/`PinBuffer` to `IFrameStoreAccess`.*** The most faithful reading of the
original comment, and the heaviest: two implementations to grow, a shared contract suite to extend,
and every residency and budget question answered a second time for a second kind of thing.

***Returning descriptor bytes by value in `FeatureSet`.*** Simplest to write, and legitimate since
engines never cross the boundary. Rejected on the budget: sixty frames of descriptors resident with
nothing able to evict them under pressure, in a core whose frame store exists precisely because a
phone runs out of memory.

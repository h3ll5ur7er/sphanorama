# 0071 — The browser builds a preview, with ORB, and the page is handed its pixels

**Status:** accepted

## Context

A capture builds natively (ADR 0070): `PanoramaBuildManager` reads the session document, extracts,
pairs, solves and composes one step per `Poll`, and the photograph ring builds to within 1.23 to
1.56 bytes of the photograph it was rendered from. Nothing of that reached a phone, for three
reasons.

- **The browser's composition root held the null engines.** `bridge/runtime.h` gave the build
  `NullRegistrationEngine` and `NullCompositionEngine`, so a build in the browser failed at its first
  step with `Unsupported`. OpenCV has been in the browser's *build* since ADR 0069 and in none of its
  *runtime*: the linker dropped it, because nothing reachable called it.
- **The engine needs a detector, and the browser had none chosen.** `FeatureRegistrationEngine`
  takes ORB, AKAZE or SIFT at construction, deliberately, so that the choice is a measurement made
  by whoever composes it rather than a preference written into the engine (V7).
- **The panorama is a handle, and the page cannot resolve one.** `Panorama` answers a `PanoramaRef`,
  whose preview is a `FrameRef` into the core's store. Frames do not cross the boundary; the one
  image that did was a candidate reduced to at most 256 pixels for the review strip (ADR 0038), and a
  panorama at 256 is a strip of colour.

## Decision

1. **The runtime composes the real engines wherever OpenCV is built.** `FeatureRegistrationEngine`
   and `NearestCentreCompositionEngine`, over the same frame store as the other pixel readers,
   declared after it. `sphanorama_bridge` links `sphanorama_registration`, and with it the
   exception-handling runtime the engine's throws need (ADR 0069). The one build without OpenCV,
   `native-contracting`, keeps the null registration engine; composition needs no OpenCV and is
   real everywhere.
2. **ORB.** The browser's build makes a preview, and a preview cannot show what the slower
   detectors buy. Solving the photograph ring under WebAssembly (ADR 0069), the median frame is
   0.046 degrees out with ORB, 0.029 with AKAZE and 0.014 with SIFT; a 2048-wide equirectangular
   preview spends 0.176 degrees on a pixel, so all three are inside a quarter of one. Extracting
   twelve 640 by 480 frames under node takes ORB 155 ms single-threaded and 153 threaded, AKAZE 720
   and 470, SIFT 940 and 574 — three to six times ORB's — and a phone waits on extraction once per
   cell. The feature cap is the engine's own, 500 a frame (`kMaxFeaturesPerFrame`). A full render,
   8192 wide and 0.044 degrees a pixel, is where the choice is made again, by measurement.
3. **A finished build's preview crosses as pixels.** `IPanoramaBuildManager::PanoramaPreview(build)`
   answers the preview as a `FramePreview`: the build's own preview frame copied out of the store,
   row by row, unreduced. Unreduced because the caller already chose its size — `BuildSpec`'s
   `outputWidth`, clamped by the build to 2048 — and a second size parameter would be a second
   copy of one decision. It is refused exactly as `Panorama` is, and with the store's own status
   where the store will not fault the preview in or let go of it after; a pin left by a release the
   store refused is the build's, and `Cancel` gives it back with the rest. This is the second image
   that leaves the core, beside `CandidatePreview`, and the rule ADR 0038 stated still holds: what
   crosses is decided by what it costs, and full-resolution frames still cross only as handles.
4. **The page asks for 2048, and polls between paints.** `shell/src/clients/build` holds one
   button, the progress, and a canvas. A press starts a build, polls it once per turn of the
   event loop — each poll is one step on the core's thread, so neither the core nor the page is
   ever held for a build — shows the stage and the fraction while it runs, and paints the preview
   when it completes. A refused start, a failed build and a core that stops answering each say why,
   and none leaves an earlier picture on screen under the new words.

## Consequences

- **The module carries OpenCV now, measured.** By `tools/size_budget.py`, which CI enforces:

  | | before | after | change |
  | --- | --- | --- | --- |
  | single-threaded module, gzipped | 110,149 | 666,888 | +556,739 |
  | threaded module, gzipped | 123,910 | 697,153 | +573,243 |

  against budgets of 8 and 10 MB. The single-threaded figure is what ADR 0069 predicted from the
  probe, to within a kilobyte. All of it at `-O3`, which ADR 0069 records as its own change.
- **The browser floor moves as ADR 0069 said it would**: the module now links WebAssembly's
  exception handling, which needs Chrome 95, Firefox 100 and Safari 15.2. Only Chrome and Firefox
  move, by versions from 2021 and 2022.
- **The deploy compiles OpenCV.** It builds only the module, and the module links it now — about
  eight minutes, since the WebAssembly objects rebuild on a cache hit (ADR 0069). The deploy still
  restores CI's cache and saves none.
- **What crosses costs 8 MB, three times.** A 2048 by 1024 RGBA preview is 8 MB in the core's copy,
  again on the wire and again in the page's canvas, briefly at once. The frame store's ceiling
  does not see the first two; the page asks for one preview per press, and a press while one is in
  flight starts nothing.
- **Not measured on a phone**, and this is what makes it measurable: how long a sphere takes to
  build, and the heap a build peaks at — the feature sets held across polls and the preview. Those
  are the figures Phase 2's exit asks for per device class, and the next thing a phone answers.
- **A build while a capture runs is allowed and not designed for.** Between polls a build pins
  nothing, so a capture proceeds; a new `Begin` empties the store, and the build then fails as one
  whose frames went (ADR 0070). The button does not wait for a capture to end.
- **The kept lens still has no writer** (ADR 0067): the document names no camera, so a build cannot
  say whose lens it fitted.

## Rejected alternatives

***SIFT, because it is the most accurate.*** It is, by a factor of three in the median — and the
preview cannot show it, while the person holding the phone waits for every frame of it. The choice
is the composition root's, one line, and a full render that can show a hundredth of a degree is
where it should be made again.

***Reduce the panorama through the frame-preview engine.*** It exists to make a frame small enough to
look at, and it would — to 256 pixels, which is its cap for a reason that holds for a strip of
thumbnails and not for one panorama. Raising its cap for one caller would make a budget into a
suggestion; a second engine to reduce an image the build already drew at the size the page asked for
would be a second answer to one question.

***Hand the page the `PanoramaRef` and let it read the store.*** The page has no store: the frame
store lives in the worker beside the core, and its bytes are the core's to give. A JavaScript path
into the heap for one call is the boundary this architecture exists to keep.

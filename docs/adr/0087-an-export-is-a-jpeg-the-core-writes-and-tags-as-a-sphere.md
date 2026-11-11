# 0087 — An export is a JPEG the core writes and tags as a sphere

**Status:** accepted; the first of two steps. This one writes the file; the next gives the browser a
port to hand it to and the page a button.

## Context

A capture builds into a preview panorama the page draws (ADR 0071), and nothing takes it off the
page. `IProjectManager::Export` has been in the contracts since the architecture was written,
answering `Unsupported` with "nothing to export until the build pipeline lands". The roadmap's Phase
2 list closes on it: "`ProjectManager` export: JPEG/AVIF with XMP `GPano`", and Phase 2's exit asks
for a file that Google Photos and a WebXR viewer open as a sphere.

Three things stood in the way.

- **Where the panorama is.** `Export` took a `BuildId`, but the build is `PanoramaBuildManager`'s,
  and managers never call managers. `ProjectManager` had no way to turn the id into pixels.
- **Who encodes.** `IImageCodecAccess` is a resource access, and the browser's encoders —
  `OffscreenCanvas.convertToBlob`, a canvas's `toBlob` — answer asynchronously. A port answers
  within the call: that is the rule `ICameraAccess::CaptureBurst` was removed for (ADR 0018). There
  is no synchronous JPEG encoder in a worker to lend the core.
- **What makes it a sphere.** A JPEG of an equirectangular picture opens in a viewer as a very wide
  photograph unless it carries Google's GPano XMP properties, in an APP1 segment before the image
  data.

## Decision

- **`Export(project, panorama, spec)` takes the panorama**, the `PanoramaRef` that
  `IPanoramaBuildManager::Panorama(build)` answers, rather than a build id. The client sequences the
  two calls, as it does a build's gaps and a fill (ADR 0086). Its `preview` is what is written,
  since a build has nothing else yet; the core holds that frame until the build is cancelled or
  another starts, so the handle is good for the export.
- **The core writes the JPEG**, with stb's `stb_image_write.h`, in a resource access of its own,
  `StbImageCodecAccess`, which is the same in every build. It reads the frame through the frame
  store, which the contract's `FrameRef` names and which the layer rules already let a port reach
  (ADR 0021), checks the bytes the handle claims against the bytes the store holds, and leaves the
  frame unpinned in the tier it found it in. The store says nothing of a frame's format, so a handle
  naming the wrong one over as many bytes is written as what it names. A side past 65535, which a
  JPEG's sixteen-bit header cannot say and stb writes without asking, and a frame past what stb's
  `int` index reaches are refused before the writer is called. It writes JPEG only: AVIF and PNG
  answer `Unsupported`, and so does `Decode`, which nothing calls. The browser never implements it,
  so its header is not mirrored into TypeScript.
- **stb is fetched at a pinned commit and checked by content.** `cmake/stb.cmake` clones
  `nothings/stb` at `fede005`, a master commit from September 2025, and refuses to configure unless
  both headers it uses hash to the SHA-256 it records. The writer there is v1.16, from July 2021 and
  unchanged on master since; the `dev` branch's writer changes of November 2026 are not taken.
  `stb_image.h` decodes in the tests only, so they check the file rather than the call. Public
  domain or MIT, at the user's choice of the two.
- **The GPano block is a utility**, `utilities/gpano_xmp`: the packet for an area of an
  equirectangular sphere, and the JPEG with it spliced in after the start of image and the JFIF
  segment. An area that cannot be part of a sphere — not twice as wide as it is tall, or cropped
  past its edges — has no packet, and a packet past what one segment holds is refused. The codec
  places a frame as the whole sphere, or, where the spec names a taller one as wide as the frame, as
  a band centred on its horizon, which is what a capture without caps is. A frame narrower than the
  sphere is refused: placing it would mean choosing an azimuth nobody gave.
- **An export is always tagged.** `Export` asks the codec for the GPano block whatever the spec's
  `attachGPanoXmp` says: an equirectangular file without it opens as a very wide photograph, which
  is the one thing an export of a sphere must not do.
- **Shared where asked and possible, saved otherwise.** `ProjectManager` hands the bytes to
  `IExportAccess`: `Share` when the spec asks and `CanShare` says yes, `Save` otherwise. The browser
  has no port yet, so its runtime holds `NullExportAccess`, which refuses: an export on the page
  answers `Unsupported` rather than writing a file nowhere. That port is the next step.

## Consequences

- **A second fetched dependency in the core**, beside OpenCV. It is two headers, compiled into one
  translation unit as system headers, so its warnings are not ours; the hash check is what keeps a
  rewritten history or a moved commit from handing us other bytes under the same name. The writer,
  the codec and the GPano utility cost the release module 11,024 bytes gzipped: 689,127 with a
  codec that writes nothing, 700,151 with this one, of the 8 MB budget.
- **The encode runs on the core's thread.** Natively at `-O2`, a 2048 by 1024 frame of deliberately
  busy content encodes in 85 ms at quality 85 and 176 ms at 92, into 0.7 and 2.2 MB. What it costs
  in WebAssembly on a phone is not measured; the next step is where a phone first runs it.
- **The page cannot export yet.** The facade carries the call and the core writes the file, but the
  browser's export port refuses, so nothing reaches the user until the second step.
- **The contract's `BuildId` argument is gone**, and with it the only call that named a build
  without asking the build manager for it.
- **The codec copies a padded frame** before writing it, since the writer reads packed rows; a frame
  the store allocates packed is written from the pinned span directly.

## Rejected alternatives

**Encode in the browser and hand the core the bytes.** The browser's encoders are good, fast and
free. But they answer asynchronously, and a port answers within the call; making `Export` a sequence
of client steps to wait for one would move the encoding out of the codec's contract and into the
page, where the GPano block would follow it. The core's encoder needs no round trip and is the same
on the bench as on a phone.

**Write our own baseline JPEG encoder.** About four hundred lines of DCT, quantisation and Huffman
tables, and no dependency. It would still need a decoder to be tested against, and a defect in a
quantisation table would show as a slightly worse picture nobody could tell from a slightly worse
camera. stb's writer is in very wide use and is one header.

**OpenCV's `imgcodecs`.** It would bring libjpeg in through the dependency already present, but it
is outside ADR 0005's six modules, and libjpeg is many times the size of a writer this export needs
one function of.

**PNG.** It needs no lossy coder and stb writes it too. But a panorama is large as a lossless file,
and some sphere viewers read GPano from a JPEG's XMP only.

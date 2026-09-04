# ADR 0028 — Markers are drawn in the box the video is painted in

Status: accepted
Date: 2026-09-04

## Context

The capture overlay draws a ring on every planned cell in the picture and an arrow toward the one
to go to. `sight()` returns a position as a fraction of the *camera frame*; the page turned that
into a CSS percentage and stopped there. Two things sit between a fraction of the camera frame and
a position on a phone, and both were being ignored.

The marker layer was inset to end where the panel begins, so that a ring was never drawn over the
status lines. The panel is more than half the screen, so this squeezed the whole field of view into
the strip above it: a cell dead ahead — the one the reticle is sitting on — drew about a third of a
screen high. The reticle was deliberately left spanning the whole picture, so the two disagreed
about where the middle was, by a lot.

`object-fit: cover` then scales the frame to fill its box and crops what hangs over. A 960×1280
camera in a portrait window on a Pixel loses about a quarter of its width, so a marker at 80% of
the frame belongs at 92% of the screen. Every marker was short of the thing it named, by more the
further out it was.

Reported from a device after a full sphere: the AR "took a bit to get used to".

## Decision

The marker layer is the picture — `inset: 0`, the same box as `#viewfinder` and `#horizon` — and
the client maps each marker through the `cover` crop with `intoViewfinder()` before writing a
percentage. The fit is measured on every paint from `video.videoWidth/Height` and the layer's own
client box, because the video has no size until the first frame decodes and the box changes shape
when the phone is turned.

An unmeasured frame or box passes the point through unchanged. Zero there means "not known yet",
and a scale derived from it would fling every marker off the screen.

The panel folds itself away when a capture starts and comes back on a tap. That is what makes the
first half possible: a marker the panel covers is now simply covered, which is honest, and it is
only tolerable because the panel is not parked over the viewfinder for the whole capture.

The arrow is *not* mapped through the fit. A ring names a point in the camera frame and has to
follow the crop to stay on it; the arrow names no point — it is a signpost placed at a chosen
distance from the centre of the screen, and cropping that would march it off the edge.

## Consequences

- The one marker whose position can be checked by eye — the cell being aimed at, under the reticle
  — now lands where the reticle is. That is the browser test: the layer's box must equal the
  video's, and the markers must move when the window changes shape, because a page handing the
  overlay no fit draws the same percentages whatever the window is doing.
- `--panel-height` and the ResizeObserver that fed it are gone. They existed only to inset the
  layer, which was the wrong fix for the right complaint.
- `cover` still hides part of what the camera captures. That is a separate question — the app aims
  with a view narrower than the frame it records — and is left as it is rather than letterboxed,
  because the acceptance cone is centred and the crop takes from the edges. Written down because it
  is a real limitation and not an oversight.
- The arrow carries how far there is to turn (`awayDeg`), and does not point at a cell that has
  already been captured. Guidance keeps naming a nearest node once the sphere is covered, which is
  the right answer to a different question; a finished sphere was still sending the user across the
  room. See ADR 0027 for why guidance answers that way.

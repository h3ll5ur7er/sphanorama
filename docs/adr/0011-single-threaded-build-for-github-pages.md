# ADR 0011 — Two WASM builds; GitHub Pages gets the single-threaded one

**Status:** accepted

## Context
Deployment is GitHub Actions to GitHub Pages. Pages serves static files and **cannot set response
headers**, so it cannot send `Cross-Origin-Opener-Policy` / `Cross-Origin-Embedder-Policy`. Without
those, the browser withholds `SharedArrayBuffer`, and without that there are no WASM threads.

`docs/01` already listed cross-origin isolation as a constraint, but as one risk among several.
With Pages as the actual target it stops being a risk and becomes the default condition.

The failure mode is worse than it looks, and the browser test now pins it: a **threaded** build
served without isolation does not degrade to running serially. It never finishes initialising —
the runtime waits on a `SharedArrayBuffer` the browser will not hand out, and the page hangs with
no error at all. A blank screen, no console message, nothing to debug from a phone.

## Decision
Produce and test **two** artifacts from one source tree:

- `wasm-release` — single-threaded. This is what ships to Pages.
- `wasm-release-threaded` — `-pthread`. For hosts that can serve COOP/COEP, and for measuring what
  threading is worth.

Neither is a fallback; both are supported configurations with the same correctness. The capability
probe answers from the build it is in, never from what the host offers, so a caller cannot size a
thread pool that cannot exist.

The browser suite serves each artifact both ways and asserts the outcome, including that the
threaded build fails to become usable without isolation — so shipping the wrong one is a red build
rather than a blank screen.

## Consequences
- Threads are not available in the default deployment. Every stage that would parallelise must
  have a serial path that is correct and merely slower, and the serial path is the reference.
- The performance targets in `docs/06` Phase 4 must be stated for the single-threaded build, since
  that is what users get. Threaded numbers are an upper bound, not the headline.
- Two artifacts to build, size-budget and test. Cheap: it is one extra preset and one type-list
  entry in the browser suite.
- If threading turns out to matter enough, the escape hatch is a service-worker shim that re-serves
  responses with the headers. It works, and it costs a first-load registration plus a reload, and
  it fails in exactly the contexts hardest to debug. Not now; the assertion above is what tells us
  when it is worth revisiting.

## Rejected
- *Ship the threaded build and let it degrade.* It does not degrade. It hangs.
- *Adopt the COOP/COEP service-worker shim immediately.* Extra moving part, a worse first load, and
  it would let us skip proving the serial path works — which is the path most users will run.
- *Move off Pages to a host that can set headers.* Possible later; it trades a free, zero-ops
  deployment for threads we have not yet shown we need.

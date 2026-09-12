# Sphanorama

On-device photo-sphere capture PWA. C++20 core → WebAssembly, thin TypeScript shell, Python
tooling. Decomposed by volatility (iDesign).

**Read `.claude/skills/sphanorama-engineering/SKILL.md` before working here.** It carries the
layer rules, the TDD workflow, contract discipline, repo structure and the definition of done.
Rationale is in `docs/00-principles.md` and `docs/03-architecture.md`.

**Review every PR with `.claude/skills/sphanorama-review/SKILL.md`.** It spawns reviewer subagents,
one per lens, against the mistakes this codebase has actually made — and each publishes its
findings to the pull request, where the answers go too. Reviews are run here rather than bought
from a bot, and they are held in the open so the reasoning outlives the session that produced it.
Expect several rounds, and change the scope at the end. On PR #49 every round after the first, up
to the thirteenth, reviewed a commit range and found its worst defect inside the previous round's
fix — which is the argument for running another rather than for stopping. Round 14 reviewed the *whole branch against main* for the
first time and found two defects that predate the branch entirely — a quaternion norm overflow and
an unbounded frame read — because a range diff cannot see code nobody touched.

The four things that are most expensive to get wrong:

1. **Write the test first.** Correctness here is invisible to the eye and the target device is a
   phone. `docs/00-principles.md` §0.2 lists the invariants worth reaching for when the expected
   output isn't knowable in advance.
2. **Respect the layers.** Clients call managers only; managers never call managers; engines are
   stateless and touch only the compute and frame-store resource accesses. CI fails on a violating
   include edge.
3. **Update the docs in the same commit** as the change that invalidates them, and write an ADR for
   anything that adds a component, changes a contract, adds a dependency, or takes a rule exception.
4. **Before adding a component, name the volatility it absorbs.** If it's already in
   `docs/02-volatility-map.md`, extend the existing owner instead.

Status: Phase 0's exit criterion is met. The native and WASM builds, the PWA shell, the three
managers, the generated boundary (contracts mirror, wire codec, facade dispatch) and the Pages
deploy are in and green. A phone opens the app, the core plans a real tessellation for the lens the
page reports, and the reticle follows guidance that came back from `CaptureSessionManager` — pose,
coverage and acceptance are all decided in the core.

**What is real.** Five of the six engine contracts have a real implementation — `CoveragePlanner`
(rings), `Pose` (orientation), `FramePreview` (box), `FrameQuality` (sharpness) and now
`Registration`, in part. `Registration` needs the care of a qualified sentence: **two** of its three
methods are implemented (`ExtractFeatures` and now `EstimatePairwise`), it exists only where OpenCV
does so a browser build still gets the null one, and no composition root selects it yet — it is
reached from tests. `Refine` still refuses. **The pair estimator is now scored against a dataset**,
which is what that qualification was waiting for: on a twelve-frame synthetic ring, against a sensor
prior perturbed three degrees, AKAZE and SIFT register all eleven consecutive pairs and ORB eight,
with medians under a tenth of a degree (ADR 0056, and the roadmap's table; ADR 0057 retracts the
figures published before the prior was perturbed — the first harness handed the estimator the exact
truth and was measuring itself). The three pairs ORB declines are not a defect, and they are not
alike: two return a rotation backed by a minority (20 of 141, 13 of 154) and the third gathers no
consensus and is refused. Under the *truth* rotation those three have 11, 19 and 13 correspondences
of 128, 141 and 154 behind them, so where the search answers it finds as many inliers as truth
itself. `Composition` is
untouched, which is the rest of what Phase 2 is for. This line said Phase 1 until Phase 2
actually started; Phase 1 is the guided capture, whose exit criterion stands at two of three
conditions on one device (see the roadmap) — far enough along that stitching is the next thing to
build, not finished.

**Phase 2 has started at the bottom.** `utilities/camera_model` projects a direction to the pixel it
lands on and back again, through Brown-Conrady distortion, and it is the first code in this
repository to read a field of `Intrinsics` — the struct two engine contracts have been passing
around unopened since the architecture was written. Every way of having no answer is a refusal
rather than a pixel: an unusable lens, a direction behind the camera, a radius past the fold where
the distortion stops being invertible, an inverse that does not land back where it started
(ADR 0046). Its iteration budget is a measured number, not a chosen one.

**OpenCV is in the build now**, fetched at a pinned commit and trimmed to ADR 0005's six modules,
native only — the WASM cross-compile has its own size budget and is still deferred (ADR 0047). Its
first use was not an engine: it cross-checked the camera model against `cv::projectPoints`, asking
our inverse to invert *their* forward map, which is a stronger statement than agreeing with their
inverse — `cv::undistortPoints` runs five passes of the fixed point we replaced, so on a wide lens
theirs is the one that is wrong. The Python tooling runs through `uv` with a committed lock file, so
`uv run tools/…` and `uv add`, never pip (ADR 0048) — with one exception that bites if you copy the
line: the dataset renderer needs `uv run --group datasets tools/…`, because numpy is in a group so
the checkers stay standard-library only (ADR 0050).

The order from here is **the harness before the algorithm**, because registration accuracy is
invisible to the eye — a rotation a degree out looks fine until the seam. The first piece of that
harness is in: `core/test/support/rotation_scoring` says how wrong a set of estimated rotations is,
and the difficulty it exists for is that a panorama's world frame is arbitrary — turn every frame by
one common rotation and it is the same panorama, so a perfect reconstruction compared frame by frame
reads as wrong by that angle on every frame at once. The gauge comes off first and the median is the
number (ADR 0049). Two things it taught. Power iteration — the obvious way to find the alignment —
exhausts a 200-iteration budget on 45.6% of wholly-unrelated inputs at sixty frames, which is the
size a real sphere plans, while Jacobi takes five sweeps at every size; that is a measurement, and
the first version of it was a rare tail stated as the norm until a reviewer re-ran it. And four of
its own tests were satisfied by their arrangement rather than by the behaviour — including a shared
fixture that could be replaced with the identity rotation without failing anything — every one found
by sabotage rather than by reading.

**The two halves of that harness meet now.** `core/test/support/synthetic_dataset` reads a rendered
dataset into a frame store — the lens, the frames, and the rotation each was taken at — which until
this existed nothing in C++ did, so the accuracy number Phase 2 exits on could not be computed
however good the scorer was. It is read against a four-frame dataset the generator itself wrote and
this repository commits, because a loader checked against a hand-written fixture is checked against
the author's idea of the format rather than the format (ADR 0053). It records the quaternion the file
spells, negative scalar part and all, since tidying the double cover would be unasked-for work on the
one field every measurement is compared against.

**And there are frames to score now.** `tools/synth_dataset.py` renders what a phone would have
captured from a panorama, with the rotation of every frame. It re-implements the lens instead of
calling `camera_model`, on purpose: a dataset rendered through the code under test cancels any error
the two share, so a wrong distortion convention would render wrong, register wrong in exactly the
compensating way, and score perfect (ADR 0050). The two are pinned to the same hand-worked decimals
instead. It taught the same lesson again in a new place — a tolerance written in colour components
was three orders of magnitude looser than the interpolation error it was meant to bound and let a
render wrong by 0.086 degrees pass, so the assertions are in degrees now, which is the unit the
thing exists to serve. Geometry only so far: noise, blur, rolling shutter, exposure, bursts and
movers are each their own increment. Which feature
detector wins is a measurement that harness makes rather than a preference the roadmap states:
SIFT's patent expired in 2020 and it has been in `features2d` since OpenCV 4.4, so it costs no new
dependency and the reason it was once excluded is gone.

`FrameQuality` is the one worth describing, because its numbers decide which frame of a burst
survives: sharpness is the variance of a Laplacian over a downscaled luma plane, exposure agreement
is measured against the rest of the burst, and `Rank` normalises before it weights so the selection
policy's knobs all turn something. `motionBlur` stays zero and says why — smear in pixels needs an
exposure time and a focal length the engine is not handed.

**The pixel path reaches the browser.** The call that did not fit the resident-port pattern,
`ICameraAccess::CaptureBurst`, is gone: a burst takes time, so `CaptureSessionManager` paces it
across the ticks the client already makes, over the preview frame the page keeps resident
(ADR 0018). `ArmBurst` arms one and `PeekPreviewFrame` is the whole of it — the page draws the
viewfinder into a canvas and transfers the buffer to the worker the core runs in, where
`PeekPreviewFrame` copies it into the frame store (ADR 0019, ADR 0021). An end-to-end test drives
that in a real browser.

**A burst's frames are comparable, and its memory is bounded.** They share an exposure where the
camera can hold one: the page applies the locks and confirms them by reading the mode back before
arming, and `SetLocks` refuses a lock it has not been told is held (ADR 0022). The frame store's
browser ceiling is read from `navigator.deviceMemory` rather than stated, so it scales with the
machine, and the policy above it is in — `CaptureSessionManager` cools a cell the moment its burst
is ranked, so a sphere larger than the store still captures and `Allocate`'s refusal is the
backstop rather than the first thing a capture hits (ADR 0023).

**Nobody presses anything.** The core counts a two-second dwell on a held cell and says `Fire`, and
the page arms on it — the decision is the core's and the call is the client's, because a burst is
paced by the client's ticks (ADR 0043). A `Fire` nobody could act on comes round again, since there
is no shutter left to fall back on.

**And a capture needs a motion sensor.** Without one, `Begin` and `Resume` refuse before they open
a camera, and the page says what is required and what is missing — a sphere whose cells are
labelled with directions nobody measured is worse than a message (ADR 0044).

**A capability is re-asked where it is consumed**, rather than remembered from `Open`: `ArmBurst`
reads the camera again after applying the locks, because pinning an exposure is what drops a camera
to half its frame rate. The page keeps that resident port true by re-reporting the camera it has
just changed — and by refusing to report one it is no longer holding (ADR 0045).

See `docs/06-roadmap.md`.

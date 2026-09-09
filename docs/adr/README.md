# Architecture decision records

One file per decision that is expensive to reverse. Format: context, decision, consequences,
and the alternative we rejected and why.

| ADR | Decision |
| --- | -------- |
| [0001](0001-decompose-by-volatility.md) | Decompose by volatility (iDesign), not by function |
| [0002](0002-cpp-wasm-core-ts-shell.md) | C++/WASM core owns business logic; TypeScript supplies resource-access adapters |
| [0003](0003-candidate-sets-not-frames.md) | A capture cell owns a set of candidates, not a frame |
| [0004](0004-build-as-incremental-graph.md) | A build is a fingerprinted DAG, not a pipeline run |
| [0005](0005-opencv-piecemeal-not-stitching-module.md) | Use OpenCV algorithms piecemeal; do not use its `stitching` module |
| [0006](0006-no-exceptions-result-type.md) | `Result<T>` everywhere; no exceptions across layers or the WASM boundary |
| [0007](0007-tests-and-docs-are-gated.md) | Tests and documentation are gated in CI, not left to discipline |
| [0008](0008-contracts-are-the-include-path.md) | One interface per header; `contracts/cpp` is the include root |
| [0009](0009-the-cpp-header-is-the-idl.md) | The C++ header is the IDL; a strict parser generates the TypeScript mirror |
| [0010](0010-resource-access-contract-suites.md) | Resource access is verified by one shared contract suite per interface |
| [0011](0011-single-threaded-build-for-github-pages.md) | Two WASM builds; GitHub Pages gets the single-threaded one |
| [0012](0012-c-abi-boundary-not-embind.md) | The WASM boundary is a C ABI over the shared heap, not Embind |
| [0013](0013-generated-binary-codec.md) | The boundary marshals a generated binary codec, not JSON or FlatBuffers |
| [0014](0014-synchronous-ports-over-a-resident-host.md) | Resource-access ports are synchronous over a resident host; the composition root is exempt from the layer rules |
| [0015](0015-absolute-orientation-in-the-imu-sample.md) | `ImuSample` carries an optional absolute orientation; the browser adapter fills it and converts the frame |
| [0016](0016-pose-state-is-a-value-the-manager-owns.md) | Pose state is a contract value the manager owns; `IPoseEngine` is a pure function of it |
| [0017](0017-the-motion-port-reports-the-viewfinders-attitude.md) | The motion port reports the viewfinder's attitude as a quaternion, screen rotation included |
| [0018](0018-the-burst-is-paced-by-the-manager-over-a-resident-frame.md) | A burst is paced by the manager across client ticks over a resident preview frame, not Asyncify |
| [0019](0019-the-core-runs-in-a-worker.md) | The core moves to the worker 04 §4.1 always specified; the resident host moves with it |
| [0020](0020-the-spill-destination-is-a-seam-inside-the-frame-store.md) | Where a spilled frame goes is a seam inside the frame store, not a resource-access port beside it |
| [0021](0021-the-pixel-path-crosses-by-transfer-and-lands-in-the-frame-store.md) | Pixels cross to the worker by transfer; the camera port allocates them in the frame store, and a peeked frame is owned |
| [0022](0022-a-lock-is-confirmed-before-a-burst-and-released-after-it.md) | A camera lock is applied and confirmed by the page before arming; taking one reads that state, releasing one is posted |
| [0023](0023-a-committed-cell-is-cooled-by-the-session.md) | A committed cell's frames are cooled by the capture session; the store owns the tier and the refusal |
| [0024](0024-orientation-and-rates-are-fused-and-the-bias-is-state.md) | The pose engine fuses an attitude with gyroscope rates when both arrive, and `PoseState` carries the learned gyro offset |
| [0025](0025-a-sample-says-whether-its-rate-was-measured.md) | `ImuSample` says whether its rate was measured; the browser adapts `rotationRate` and the capability stops deciding |
| [0026](0026-candidates-come-back-ranked.md) | `Candidates(node)` hands a cell back ranked best-first, so a review client can name the automatic pick without deciding what best means |
| [0027](0027-guidance-aims-at-what-is-missing.md) | `Locate` takes the coverage state and says `SphereDone` when nothing is left — its targeting rule partly superseded by 0041 |
| [0028](0028-markers-are-drawn-in-the-box-the-video-is-painted-in.md) | The overlay's markers are drawn in the video's own box and mapped through the `object-fit: cover` crop, and the panel folds out of the picture while a capture runs |
| [0029](0029-a-session-is-resumed-by-the-manager-that-owns-one.md) | `Resume` moves to `ICaptureSessionManager`, the frame store can `Adopt` frames a dead store spilled, and the session document is written at every cell |
| [0030](0030-the-spill-tier-is-resident-and-carries-its-own-index.md) | The OPFS spill tier has a fixed preferred name and a sibling index, so a reload can find the frames its session document names |
| [0031](0031-a-lock-is-asked-for-one-at-a-time.md) | Each lock is negotiated in a constraint set of its own, with an exposure time and a `single-shot` fallback, and a refusal is remembered for the track |
| [0032](0032-a-burst-waits-for-the-camera-before-its-first-frame.md) | A burst's first frame waits `BurstSpec::settleMs` after arming, so the camera has converged on the locks that arming applied |
| [0033](0033-what-the-camera-offers-is-reported-not-consulted.md) | The track's mode lists are read at open and reported against a refusal; they never decide what the camera is asked for |
| [0034](0034-a-new-capture-empties-the-tier-a-resumed-one-does-not.md) | A new capture empties the spill tier; a resumed one does not |
| [0035](0035-the-tier-says-which-capture-it-is-holding.md) | The spill tier carries a token saying which capture it holds; the session document records it and `Resume` refuses a document that names another |
| [0036](0036-a-resumable-capture-is-visible-in-the-project-listing.md) | `ProjectSummary` carries `hasSession`, so the page can offer a resume without attempting one |
| [0037](0037-a-cell-keeps-only-what-it-can-still-rank.md) | A cell keeps the best eight candidates and forgets the rest, because ranking faults the whole cell back into the heap |
| [0038](0038-a-frame-leaves-the-core-reduced-and-a-new-engine-reduces-it.md) | A frame leaves the core reduced to a preview, and a new engine (V16) is what reduces it |
| [0039](0039-a-refused-resume-keeps-its-offer-unless-only-a-build-could-change-it.md) | A refused resume keeps its offer unless nothing a press can do would change the answer — `Unsupported`, which waits for a new build, and since 0044 `SensorUnavailable`, which waits for a reload; the withdrawal lives in the tab and is never written down |
| [0040](0040-a-selection-is-read-back-from-the-core.md) | `IProjectManager` gains `GetSelection`, so the review strip shows the recorded pick rather than a copy of its own writes |
| [0041](0041-aim-decides-which-cell-a-burst-belongs-to.md) | Guidance names the cell the camera is inside, captured or not, and `ArmBurst` refuses one it is not aimed at |
| [0042](0042-with-no-aim-coverage-decides-alone.md) | `Locate` takes a `PoseSample` and prefers the cell the camera is inside only when the pose was measured — **superseded by 0044**, which refuses the session instead |
| [0043](0043-the-dwell-that-fires-a-burst-is-the-cores.md) | The dwell that fires a burst is counted in `CaptureSessionManager` and reported on `CaptureGuidance`; the page acts on it and draws the ring from the same number |
| [0044](0044-a-capture-needs-a-motion-sensor.md) | A capture requires a motion sensor: `Begin` and `Resume` refuse without one and the page says what is missing, superseding 0042's degraded path |
| [0045](0045-a-capability-that-changes-is-re-asked.md) | A capability is re-asked where it is consumed: `ICameraAccess` grows `Capabilities()` and `ArmBurst` calls it after `SetLocks`, while the page keeps the resident port true by re-reporting the camera it has just changed |
| [0046](0046-the-lens-is-a-utility-and-an-uninvertible-pixel-is-refused.md) | The lens is a utility rather than an engine, and every way of having no answer — an unusable lens, a direction behind the camera, a radius past the fold, an inverse that does not land — is a refusal rather than a pixel |
| [0047](0047-opencv-is-fetched-pinned-and-trimmed-and-earns-its-place-on-a-cross-check.md) | OpenCV is fetched from source at a pinned tag, trimmed to ADR 0005's six modules, linked natively only — and its first use is a cross-check of the camera model against `cv::projectPoints` rather than an engine |
| [0048](0048-the-python-tooling-runs-through-uv.md) | The Python tooling runs through `uv` with a committed lock file, so the interpreter and any future dependency are facts about the repository rather than about a machine |
| [0049](0049-accuracy-is-measured-with-the-gauge-removed.md) | Registration accuracy is scored with the global gauge rotation removed first, by Markley's chordal average found with Jacobi rather than power iteration, and the headline number is the median |
| [0050](0050-the-dataset-renderer-re-implements-the-lens-on-purpose.md) | The synthetic dataset renderer re-implements the lens rather than calling the core, because a dataset rendered through the code under test cancels any error the two share — and numpy arrives in an opt-in group so the checkers stay standard-library only |

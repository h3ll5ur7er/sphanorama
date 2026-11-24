# Codebase review — `main` at e1cd175, 2026-11-24

This is a point-in-time review of the whole repository, not of a pull request. The line numbers
match e1cd175; once the code has moved, read a location as "near here". Each finding is meant to
become a PR, or a decision recorded somewhere, and then be struck from this file. When every
finding is gone, delete the file.

**62 findings: none high, 25 medium, 37 low.** No finding is reachable through ordinary use of the
shipped app in a way that crashes it, loses a capture or gives a wrong panorama. Where the medium
findings cluster:

- **Saved sessions.** A non-finite number, a refused checkpoint or an edited document can each
  leave a capture that cannot be resumed or built, or that crashes the core when it is resumed
  ([CAP-1](#cap-1), [CAP-3](#cap-3), [RA-1](#ra-1)).
- **The page.** Two capture loops can run at once, the camera can stay live after a refused start,
  and a desktop with no motion sensor starts a capture that never captures
  ([SH-1](#sh-1)–[SH-3](#sh-3)).
- **Burst ranking.** It favours whichever frame was taken first ([ENG-1](#eng-1)).
- **What reaches `main`.** Deploys do not wait for CI, and several CI checks can pass while
  checking nothing ([TL-1](#tl-1)–[TL-4](#tl-4)).
- **Docs.** Four sentences that someone would act on are now false ([DOC-1](#doc-1)–[DOC-4](#doc-4)).

## How it was done

Eight reviewers, each with one area and one or two lenses from
`.claude/skills/sphanorama-review/SKILL.md`, read their area in full:

- engines;
- `CaptureSessionManager`;
- `PanoramaBuildManager` and `ProjectManager`;
- resource access and utilities;
- the WASM boundary and its generators;
- the TypeScript shell;
- tooling, CI and the build;
- docs and ADRs.

They confirmed findings without rebuilding anything or touching the tree. C++ was confirmed with
standalone GoogleTest probes linked against the built core. The boundary findings were driven in
headless Chromium against the shipped `build/wasm-release` module, and the shell findings in
vitest/happy-dom harnesses. Python and Node probes covered the tooling.

Every medium finding was then checked a second time before it went into this file: re-run where
a probe existed, read against the code where none did. Two findings that two areas reported
independently are merged.

Severity:

- **high:** a crash, data loss, a security hole or a wrong panorama, reachable in the shipped browser
  build or the documented native use;
- **medium:** reachable only through unusual input, a caller the contracts allow but the page does
  not make yet, or a document wrong in a way someone will act on;
- **low:** a cleanup, a test that cannot fail, or prose nobody acts on.

Verdicts:

- **Confirmed:** something was run and printed the failure. *(re-run)* means the probe was run a
  second time for this report; *(read)* means the finding was checked against the code a second
  time.
- **Reasoned:** an argument from the code, with what would settle it.

## Fix first

These are ordered by how likely a real user or a real push is to hit them, not by severity alone.

1. [SH-2](#sh-2): the camera stays live after the core refuses to start, for example on iOS when
   motion access is declined.
2. [ENG-1](#eng-1): the first frame of every burst gets a perfect exposure score. This is the shipped
   path on any camera that will not lock exposure, which includes the Pixel recorded in ADR 0031.
3. [SH-3](#sh-3) and [SH-1](#sh-1): a capture that can never capture, and two loops after a camera
   recovery.
4. [TL-1](#tl-1) and [TL-3](#tl-3): a red `main` deploys, and a stray `test.only` turns the deploy's
   only browser gate green.
5. [CAP-1](#cap-1), [CAP-3](#cap-3) and [RA-1](#ra-1): saved sessions that cannot be read back, that
   name dropped frames, or that crash the core when resumed.
6. [BND-1](#bnd-1): a JavaScript exception out of a host leaks the WASM stack until the core
   corrupts itself.
7. [DOC-1](#doc-1)–[DOC-4](#doc-4): four quick edits that stop sentences in the most-read documents
   from misleading.

## All findings

| ID | Sev | Area | Finding | Verdict |
| --- | --- | --- | --- | --- |
| [ENG-1](#eng-1) | medium | engines | Exposure agreement depends on capture order; an outlier taken first ranks first | Confirmed (re-run) |
| [CAP-1](#cap-1) | medium | capture | A non-finite double makes the session document unreadable, for the whole sphere | Confirmed (re-run) |
| [CAP-2](#cap-2) | medium | capture | A discarding retake forgets offered frames that belong to the caller | Confirmed (re-run) |
| [CAP-3](#cap-3) | medium | capture | Frames are forgotten before the document stops naming them | Confirmed (re-run) |
| [BLD-1](#bld-1) | medium | build/project | Three different tests of "the project exists" disagree after a `Delete` | Confirmed (re-run) |
| [RA-1](#ra-1) | medium | resource access | `Adopt` takes geometry `Allocate` refuses; the budget sum overflows and `Pin` crashes | Confirmed (re-run) |
| [RA-2](#ra-2) | medium | resource access | `Adopt` bounds an identity only at zero; the counter wraps and reissues a frame | Confirmed (re-run) |
| [BND-1](#bnd-1) | medium | boundary | A JS exception from a host leaks the WASM stack until the core corrupts itself | Confirmed (re-run) |
| [BND-2](#bnd-2) | medium | boundary | Threaded build: two heap views go stale when another thread grows memory | Mechanism confirmed, trigger reasoned |
| [BND-3](#bnd-3) | medium | boundary | The browser project store does not keep a title's bytes | Confirmed (re-run) |
| [BND-4](#bnd-4) | medium | boundary | The generator accepts explicit enum values and mirrors them by index | Confirmed (re-run) |
| [SH-1](#sh-1) | medium | shell | Two capture loops run after the camera-recovery path | Confirmed (read) |
| [SH-2](#sh-2) | medium | shell | The camera keeps streaming after the core refuses `Begin`/`Resume` | Confirmed |
| [SH-3](#sh-3) | medium | shell | A sensorless desktop starts a capture that can never capture | Adapter confirmed, page reasoned |
| [SH-4](#sh-4) | medium | shell | A muted camera track can bank a frozen frame | Reasoned |
| [TL-1](#tl-1) | medium | tooling | Nothing stops a red `main` from deploying | Confirmed (read) |
| [TL-2](#tl-2) | medium | tooling | The layer checker sees one spelling of an include | Confirmed (read) |
| [TL-3](#tl-3) | medium | tooling | A committed `test.only` turns the browser suite green | Confirmed (read) |
| [TL-4](#tl-4) | medium | tooling | `ctest` passes when no tests exist | Confirmed (read) |
| [TL-5](#tl-5) | medium | tooling | Every action is pinned by a movable tag; the build job holds deploy permissions | Reasoned |
| [TL-6](#tl-6) | medium | tooling | Dependency pins are cache variables, so a bump is ignored by an existing tree | Confirmed (read) |
| [DOC-1](#doc-1) | medium | docs | Six places say no composition root selects the real registration engine | Confirmed (read) |
| [DOC-2](#doc-2) | medium | docs | docs/04 §4.2 describes a spill tier, a heap tier and a probe that do not exist | Reasoned |
| [DOC-3](#doc-3) | medium | docs | "The one image that leaves" is false since `PanoramaPreview` | Confirmed (read) |
| [DOC-4](#doc-4) | medium | docs | "Resource access is implemented twice" is stated as the rule and is not | Confirmed |
| [ENG-2](#eng-2) | low | engines | The RANSAC budget comment is wrong since ADR 0056; `Acceptance` passes on ~87% of seeds | Confirmed |
| [ENG-3](#eng-3) | low | engines | The prior-bound test passes an engine that bounds only the answer | Confirmed |
| [ENG-4](#eng-4) | low | engines | The box preview is stricter about the last row than the other pixel readers | Reasoned |
| [ENG-5](#eng-5) | low | engines | `ReadDescriptors` accepts a pitch narrower than a descriptor | Reasoned |
| [CAP-4](#cap-4) | low | capture | The decoder accepts two candidates under one identity, or one frame twice | Confirmed (re-run) |
| [CAP-5](#cap-5) | low | capture | `Begin` reports a project-store read failure as `NotFound` | Reasoned |
| [CAP-6](#cap-6) | low | capture | Rollbacks ignore a refused `Forget`, unlike `Discard` and `Trim` | Reasoned |
| [CAP-7](#cap-7) | low | capture | Four contract sentences the implementation does not keep | Confirmed |
| [BLD-2](#bld-2) | low | build/project | A refused `Cancel` overwrites why a failed build failed | Confirmed (re-run) |
| [BLD-3](#bld-3) | low | build/project | Three comments say `ProjectManager` reads only the candidate counter | Reasoned |
| [BLD-4](#bld-4) | low | build/project | `Start`'s refusal when the tier cannot answer is untested | Reasoned |
| [RA-3](#ra-3) | low | resource access | Nothing tests `Demote`'s refusal of a pinned frame | Confirmed |
| [RA-4](#ra-4) | low | resource access | "Generation zero means no durable tier" is not enforced against the sink | Confirmed |
| [RA-5](#ra-5) | low | resource access | `AngleBetween` does not keep the header's degenerate-input promise | Confirmed |
| [RA-6](#ra-6) | low | resource access | `ManualClock::AdvanceNs` overflows | Reasoned |
| [BND-5](#bnd-5) | low | boundary | A non-const reference parameter is mirrored as an input and its output dropped | Confirmed (re-run) |
| [BND-6](#bnd-6) | low | boundary | A `final` or derived `@boundary` interface vanishes without an error | Confirmed (re-run) |
| [BND-7](#bnd-7) | low | boundary | `float` is decoded as an unchecked narrowing | Confirmed (re-run) |
| [BND-8](#bnd-8) | low | boundary | The TS wire strips a leading BOM and replaces lone surrogates | Confirmed |
| [BND-9](#bnd-9) | low | boundary | `wire_kind()` is dead code pinned to a wire width nothing uses | Confirmed |
| [SH-5](#sh-5) | low | shell | `void remote.flush()` rejects unhandled once the worker is dead | Reasoned |
| [SH-6](#sh-6) | low | shell | A refused `begin` leaves an empty titled project behind | Reasoned |
| [TL-7](#tl-7) | low | tooling | googletest is pinned by tag only | Confirmed |
| [TL-8](#tl-8) | low | tooling | The dist freshness check misreads ordinary Playwright filters | Confirmed |
| [TL-9](#tl-9) | low | tooling | With `SPHANORAMA_CORE_PROFILE` set, the single-threaded core is never compared | Confirmed |
| [TL-10](#tl-10) | low | tooling | The bundle's freshness ignores some of its inputs | Confirmed |
| [TL-11](#tl-11) | low | tooling | Both line-scanning C++ checkers skip `.inl` and `.cxx` | Confirmed |
| [TL-12](#tl-12) | low | tooling | CI's accuracy step re-enables `set -e` before its own explanation | Confirmed |
| [TL-13](#tl-13) | low | tooling | The size budget passes an empty profile | Confirmed |
| [TL-14](#tl-14) | low | tooling | `setup_emsdk.sh` fetches and never checks out; the version is written thrice | Reasoned |
| [DOC-5](#doc-5) | low | docs | ADR supersession links missing or one-sided | Confirmed |
| [DOC-6](#doc-6) | low | docs | The README's preset count and `PATH` list are wrong | Confirmed |
| [DOC-7](#doc-7) | low | docs | docs/05 names tooling that does not exist | Confirmed |
| [DOC-8](#doc-8) | low | docs | The list of CI checks claims to be complete and is not | Confirmed |
| [DOC-9](#doc-9) | low | docs | Stale planned-cell counts in the roadmap | Confirmed |
| [DOC-10](#doc-10) | low | docs | The roadmap says detector cost in WASM is unmeasured; it was measured | Confirmed |
| [DOC-11](#doc-11) | low | docs | Smaller stale sentences | Confirmed |

---

## Engines

### ENG-1

**Exposure agreement depends on capture order, so an exposure outlier taken first ranks first.**
medium · Confirmed (re-run)

- **Where:**
  - `core/src/engines/frame_quality_engine/sharpness_frame_quality_engine.cpp:309-325`, which `Rank`
    reads at `:358-362`;
  - the sequencing, in `core/src/managers/capture_session_manager/capture_session_manager.cpp:1233-1237`;
  - pinned as intended by `core/test/engines/frame_quality_engine_test.cpp:496`.
- **What breaks:**
  - `Score` compares a frame only with the siblings it is handed, and gives 1.0 when there are none.
    The manager scores each burst frame as it arrives, so frame 0 always gets 1.0 and frame *k* is
    compared only with frames 0…*k*−1. `Rank` never recomputes these values.
  - Five equally sharp frames with luma offsets `{+60, 0, 0, 0, 0}` score 1.0000, 0.7647, 0.8824,
    0.9216 and 0.9412, and rank **0** 4 3 2 1. Scored against the whole burst, frame 0 would be
    the worst.
  - ADR 0031's device agreements (1.00, 1.00, 0.67, 0.78, 0.84) are exactly this arithmetic for two
    frames at one exposure and three at another. The "metering settled" reading there is the
    averaging, not the camera. That device refused the exposure lock, so this is the shipped path.
- **Why it is not caught:**
  - Every agreement test uses a single sibling.
  - The first-frame test's rationale ("does not penalise the first frame") describes a frame being
    favoured as if it were fairness.
- **Fix:** compute agreement over the whole set in `Rank`, which means carrying mean luma with each
  candidate (a contract change and an ADR). The alternative is for the manager to re-score the
  burst once it is full.
  Tests:
  - an outlier taken first does not rank first;
  - reversing the capture order does not change the ranking.

### ENG-2

**The RANSAC budget comment says a consensus below the 0.2 gate "would be refused even if found".
Since ADR 0056 it is returned unaccepted.** low · Confirmed

- **Where:** `core/src/engines/registration_engine/feature_registration_engine.cpp:1122-1126`, repeated
  in `core/test/engines/registration_engine_test.cpp:2087-2088`.
- **What breaks:**
  - `Acceptance` (`registration_accuracy_test.cpp:713`) depends on ORB answering frames 3–4 at
    20/141 inliers, unaccepted. The budget is sized for 0.2, which misses a 0.142 consensus about
    19% of the time.
  - Over 200 seeds, ORB was refused outright 26 times, so the test passes on about 87% of draw
    sequences. Any change to the draw re-rolls it.
- **Fix:**
  - Correct both comments.
  - Either size the budget for the weakest consensus the engine still answers, or record the
    fragility beside the test.
  - Pin the choice with a seed sweep.

### ENG-3

**`ThePriorBoundsTheSearchAndTheAnswerStaysInsideIt` passes an engine that bounds only the answer,
and its explanation is false.** low · Confirmed

- **Where:** `core/test/engines/registration_engine_test.cpp:1051-1102`; the comment is at `:1077`.
- **What breaks:**
  - A variant that searches freely and refuses any answer outside 45° passes this test.
  - On the checkerboard ring, that variant refuses exactly the aliased pairs the bound exists to
    stop, and the real engine registers them.
  - The comment's "a half turn sends every bearing behind the camera" is false: that half turn is
    about +Z, and 1,024 of 1,024 bearings still land in the frame.
- **Fix:**
  - Correct the comment.
  - Add a case where an alias wins on inliers while the prior is near the truth.

### ENG-4

**`BoxFramePreviewEngine` is stricter than the other pixel readers about the last row.**
low · Reasoned

- **Where:** `core/src/engines/frame_preview_engine/box_frame_preview_engine.cpp:99`.
- **What breaks:** it requires `stride*height` bytes, where composition, sharpness and extraction
  require `(h−1)*stride + rowBytes`. A store that ends a padded frame at its last row would be
  scored and composed, then refused for the thumbnail. No store pads rows today.
- **Fix:** use the subtraction form, and add an unpadded-last-row case.

### ENG-5

**`ReadDescriptors` accepts a descriptor pitch narrower than a descriptor.** low · Reasoned

- **Where:** `core/src/engines/registration_engine/feature_registration_engine.cpp:875-908`.
- **What breaks:** a pitch of 16 on 32-byte ORB rows passes every check. The matcher then reads
  overlapping half-descriptors: in bounds, but the answer is garbage. Only a caller-edited
  `FeatureSet` reaches it.
- **Fix:** refuse `pitch < descriptors.width`, as the keypoint reader does.

## CaptureSessionManager

### CAP-1

**One non-finite double in a candidate makes the session document unreadable, and the whole sphere
goes with it.** medium · Confirmed (re-run). Found independently by the capture-session and
resource-access reviewers.

- **Where:**
  - `core/src/utilities/session_document.cpp:23-27`: `Digits` writes `nan`/`inf`.
  - `session_document.cpp:167-178`: `operator>>` refuses those tokens and fails the whole document.
  - `core/src/engines/pose_engine/orientation_pose_engine.cpp:316`: the last sample's rate is copied
    onto the pose, measured or not.
  - `core/src/utilities/quaternion.cpp:212-219`: `PoseSampleDefect` never looks at the rate.
- **What breaks:**
  - A motion sample with `hasAngularVelocity = false` and a NaN rate gives a confidence-1 pose
    carrying the NaN. The checkpoint writes `… 0 0 nan 0 0 1 …` and then cannot decode it.
  - In a new tab `Resume` answers `Unsupported` and `List` reports `sessionUnreadable`. In the live
    tab `Start` and `SetSelection` refuse too, and every later checkpoint rewrites the NaN.
  - Resume's own promise is false for the same reason: "a restored candidate whose pose
    `OfferFrame` would refuse comes back unanchored" fails for a NaN confidence, a NaN component or
    `w = inf`, because the parse fails before the pose is asked.
- **Reach:**
  - The shipped shell sends zeros for a missing rate (`shell/src/access/orientation.ts:153`), and
    WebIDL `double?` excludes NaN.
  - So it takes a future motion port, or a browser firing a non-null non-finite rate. If one does,
    this becomes high.
- **Why it is not caught:** `SpecDefect` and `PlanDefect` guard the spec and plan for exactly this
  reason (ADR 0077); candidate lines have no such guard. The tampered-pose test uses only finite
  values.
- **Fix:**
  - Have the pose engine publish a rate only from a measured sample, and zero otherwise.
  - Have the document read what it writes: `std::from_chars` reads `nan`/`inf`. Or refuse to write
    a non-finite value.
  - Tests:
    - an encode/decode round trip with NaN and ±inf in every double field of a candidate;
    - a manager test feeding a NaN-rate sample and then resuming.

### CAP-2

**A discarding retake forgets the frames of offered candidates, which belong to the caller.**
medium · Confirmed (re-run)

- **Where:** `core/src/managers/capture_session_manager/capture_session_manager.cpp:223-235`
  (`Discard`), reached from `:1465`.
- **What breaks:**
  - Offer a frame to a cell, then call `RequestRetake(node, replace=true)`. `Forget` runs on the
    caller's frame, and the caller's `ResidencyOf` and `Pin` then answer "no such frame".
  - If the caller holds a pin, `Forget` refuses, and the cell stays covered by a frame the session
    does not own.
- **Why it is not caught:**
  - `Trim` (`:1160`), `Cool` (`:1113`) and `OfferFrame`'s refusal path (`:1336`) all check
    `burst_owned_`. `Discard` is the one path that does not.
  - No test offers and then retakes with `replace`.
  - The shell calls neither `OfferFrame` nor `RequestRetake`; the contract allows both.
- **Fix:** drop non-burst-owned candidates from the cell without `Forget`. Test: offer, retake with
  `replace`, and expect the cell empty and the offered frame still pinnable.

### CAP-3

**`Discard` and `Trim` forget frames before the document stops naming them, so a resume restores
candidates that point at nothing, and every later checkpoint keeps them.** medium · Confirmed (re-run)

- **Where:**
  - `capture_session_manager.cpp:1464-1469`: `Discard`, then `Checkpoint`.
  - `:1169` with `:1288`: `Trim`, then `Disarm`, which checkpoints.
  - `:1358-1363`: the same order in `OfferFrame`.
  - `:629-635`: the `Checkpoint` comment.
  - The promise is in `contracts/cpp/sphanorama/managers/capture_session_manager.h:47-48`.
- **What breaks:**
  1. A burst lands and is checkpointed.
  2. A later write of the document is refused: the store refuses, the tier generation cannot
     answer, or in the browser a tab is killed inside the document host's 250 ms flush delay.
  3. `RequestRetake(…, true)` drops the frames from the sink, and the write that would record it
     does not land.
  4. The tab dies. The next `Resume` restores the candidates, because `Adopt` cannot check the sink.
  5. The cell counts as shot, but every `Pin` fails with "nothing spilled here", and so does a build.
     The next successful write still names the dropped frames.
- **Why it is not caught:**
  - `Checkpoint` throws away the write's status, so `RequestRetake` answers `Ok`.
  - Every retake test reads the in-process document, never a resume after a refused write.
- **Fix:** write the document without the candidates first, and call `Forget` only after that write
  lands. On a refused write, keep the candidates and refuse the retake. A crash in between then
  leaves orphans that the next `Begin` clears, rather than a document naming dropped frames.

### CAP-4

**The decoder accepts two candidates under one identity, or one frame under two.**
low · Confirmed (re-run)

- **Where:** `core/src/utilities/session_document.cpp:159-197` has no uniqueness check for
  candidates, though cells get one at `:273`.
- **What breaks:**
  - **Duplicate candidate ids:** a document with two candidates sharing id 1 restores both, and the
    next ranking silently drops one. Its frame is never forgotten, so the store and the sink keep
    its bytes until a `Begin` clears the tier.
  - **One frame named twice:** a document that names one frame under two candidates is adopted once.
    - A retake that discards either candidate drops the sink copy the other still names.
    - `PanoramaBuildManager::Start` refuses that capture on every attempt.
- **Fix:** refuse repeated candidate and frame identities, next to the existing cell check.

### CAP-5

**`Begin` reports a project-store read failure as `NotFound` "no such project".** low · Reasoned

- **Where:** `capture_session_manager.cpp:250-252`. The same pattern is in
  `ProjectManager::Exists` ([BLD-1](#bld-1)).
- **What breaks:** a title read refused for any other reason answers `NotFound`, which the contract
  reserves for a missing project. ADR 0078's rule is that a failure is not absence. Browser reads
  fail only with `NotFound` today.
- **Fix:** return the store's status unless it is `NotFound`.

### CAP-6

**A rollback ignores a refused `Forget`, unlike `Discard` and `Trim`.** low · Reasoned

- **Where:** `capture_session_manager.cpp:1075` and `:1243`.
- **What breaks:** if `Forget` were refused, the pending frame's handle is dropped while the store
  goes on charging its bytes. No current store refuses here, because a pending frame is never in
  the sink.
- **Fix:** keep refused frames where `End`/`Begin` can retry them, or say why one cannot be refused.

### CAP-7

**Contract prose the implementation does not keep.** low · Confirmed

- **`RequestRetake`:** `capture_session_manager.h:249-250` says it "aborts a burst in flight". It
  aborts only a burst at the same cell (`.cpp:1456`).
- **`Candidates`:** `capture_session_manager.h:193-194` says the automatic pick is "the frame a
  build takes". An offered candidate can rank first, and ADR 0070 says offered frames are never
  built from.
- **The dwell `std::optional`:** the implementation header's comment (`:200-201`) justifies it with
  "a zero node id is a real cell". `PlanDefect` refuses id 0 since ADR 0077.
- **`Begin`:** `capture_session_manager.h:26-27` says the `NotFound` check comes first.
  `FailedPrecondition` for a session in progress comes before it.

## PanoramaBuildManager and ProjectManager

### BLD-1

**A capture can be listed, offered and resumed into a project that every build, pick and export
call says does not exist.** medium · Confirmed (re-run)

- **Where:**
  - `panorama_build_manager.cpp:48`;
  - `project_manager.cpp:50` (`Exists`), `:64` (`List`) and `:119` (`Delete`);
  - `capture_session_manager.cpp:250` (`Begin`) and `:393` (`Resume`).
- **What breaks:**
  - Run `Create`, `Begin`, `Delete`, then `End`. The checkpoint rewrites `session`, and the store
    creates the project on demand, with no title.
  - `List` then offers the capture and `Resume` succeeds. But `Start`, `SetSelection`,
    `GetSelection` and `Export` all answer `NotFound`, so the capture can never become a panorama.
- **Why it is not caught:**
  - Three managers test existence three ways: the `"title"` document (named `kTitleKey` only
    inside `ProjectManager`), "any document at all", and nothing.
  - `Delete` has no contract sentence for a project a live session is checkpointing into.
- **Reach:**
  - The shell has no `Delete` caller.
  - One narrow browser route is reasoned: a failed title persist re-queued behind a session write,
    then a kill between the two puts (`shell/src/access/document-host.ts`).
- **Fix:**
  - One existence predicate and one named title key, in a utility every manager reads.
  - `Resume` checks the title, as `Begin` does.
  - A checkpoint refuses to write into a project whose title is gone.
  - `Delete` gets a contract sentence.
  - Pin it so that `List`, `Resume` and `Start` agree.

### BLD-2

**A refused `Cancel` replaces the reason a failed build failed with "cancelled".**
low · Confirmed (re-run)

- **Where:** `panorama_build_manager.cpp:611-614`. The guard is `stage != Complete`, and it does not
  exclude `Failed`.
- **Fix:** `stage != Complete && stage != Failed`, with a test that the failure detail survives a
  refused cancel. The shell never calls `Cancel`, so nothing reaches this today.

### BLD-3

**Three comments say `ProjectManager` reads only the session document's candidate counter. `List`
decodes the whole document (ADR 0083).** low · Reasoned

- **Where:**
  - `contracts/cpp/sphanorama/managers/project_manager.h:19-20`;
  - `project_manager.cpp:68-70`;
  - `core/src/utilities/session_document.h:19-22`.
- **Why it matters:** anyone changing the document's shape looks here for its readers, and would
  miss the one that decides which capture is offered back.

### BLD-4

**`Start`'s refusal when the tier cannot say which capture it holds is untested.** low · Reasoned

- **Where:** `panorama_build_manager.cpp:69`.
- **What breaks:** `FakeSpillSink::FailGenerations` is never used in `panorama_build_manager_test.cpp`,
  so a fall-through there would refuse with the wrong message unnoticed.
- **Fix:** a test that the store's code comes back whole and no build exists.

## Resource access and utilities

### RA-1

**`Adopt` takes frame geometry that `Allocate` would refuse, the int64 budget sums then overflow,
and the next `Pin` crashes the core.** medium · Confirmed (re-run)

- **Where:** `core/src/resource_access/frame_store_access/memory_frame_store_access.cpp`:
  - `:262-290`: `Adopt` checks only `FrameByteSize > 0`.
  - `:131`: `FaultIn` checks `heap_used_ + entry.size > ceiling_`.
  - `:69`: `Allocate` uses the same sum.
- **What breaks:**
  - An RGBA8 handle of 2147483646 × 1073741825 is `INT64_MAX − 7` bytes. `Allocate` refuses it,
    `Adopt` accepts it, and two of them leave `spilledBytes == −16`, which is signed overflow.
  - `Pin` of one, with anything resident, wraps the ceiling check, and `FaultIn` asks for 9.2e18
    bytes. That throws `bad_alloc`, which is `std::terminate` under `-fno-exceptions` and an abort
    of the worker in the browser.
- **Reach:** `Resume` passes each candidate's frame from the session document to `Adopt` unchanged
  (`capture_session_manager.cpp:526-527`), and the decoder reads width and height as any `int32`.
  So a session document edited or corrupted in IndexedDB crashes the core at the first pin.
- **Fix:**
  - Ask by subtraction in both places (`size > ceiling_ − heap_used_`).
  - In `Adopt`, refuse any geometry `Allocate` would refuse, and anything larger than the ceiling.
  - A contract-suite case for the oversize adopt.

### RA-2

**`Adopt` bounds an identity only at zero. Past 2^53 the spill sink cannot tell two frames apart,
and at 2^64−1 the counter wraps and `Allocate` hands out a capture's frame as new.**
medium · Confirmed (re-run)

- **Where:** `memory_frame_store_access.cpp`:
  - `:250` checks only `valid()`.
  - `:295` steps `next_id_ = id + 1`, which wraps.
  - `:105-117` ignores `emplace`'s result.
- **What breaks:**
  - **At 2^64−1:** after adopting frame 1 and then `UINT64_MAX`, the next allocations return id 0
    (invalid), then id 1, which is the capture's frame and its pixels. The heap is charged twice,
    and writing the "new" frame overwrites the capture's copy in the sink.
  - **Past 2^53:** frames cross to the OPFS sink as doubles, so 2^53+1 and 2^53 share a slot.
- **Reach:** `Resume` refuses identities at or above 2^52 first (`LeavesHeadroom`). The store itself
  enforces neither bound, and its contract states none.
- **Fix:**
  - `Adopt` refuses `!IdentityWithHeadroom`.
  - `Allocate` refuses past `IssuableIdentity`, and treats a failed `emplace` as `Internal`.

### RA-3

**Nothing tests `Demote`'s refusal of a pinned frame. Removing it leaves all 1322 tests green.**
low · Confirmed

- **Where:** `memory_frame_store_access.cpp:195-197`, against the promise at `frame_store_access.h:20-21`.
- **What breaks if it goes:**
  - `Demote` would free the vector under a live span, a use-after-free for the engine holding it.
  - After `Release`, the next `Pin` returns an empty span for a frame the store reports resident.
- **Fix:** a typed contract test, `DemotingAPinnedFrameIsRefused`.

### RA-4

**`TierGeneration`'s "zero means there is no durable tier" is not enforced against the sink.**
low · Confirmed

- **Where:** `memory_frame_store_access.cpp:339-340`, `spill_sink.h:165-180` and `frame_store_access.h:81-88`.
- **What breaks:**
  - A conforming sink that mints 0 makes a store with a durable tier report "no tier", and `Adopt`
    then accepts its frames, the opposite of the contract.
  - `PanoramaBuildManager` refuses generation 0, so those captures would never build.
- **Why it is not reached:** the browser host and the fake both mint from 1. The rule lives in two
  adapters, not in the store whose contract states it.
- **Fix:** the store refuses a zero token, and `spill_sink.h` says it is non-zero.

### RA-5

**`AngleBetween` returns π for a degenerate input where the header promises zero.** low · Confirmed

- **Where:** `core/src/utilities/quaternion.h:12-15`. Every production caller gates its input with
  `IsUsableRotation` first, so nothing acts on it.
- **Fix:** return 0 when `UsableNorm` refuses either input, or move `AngleBetween` onto the
  header's list of exceptions.

### RA-6

**`ManualClock::AdvanceNs` overflows.** low · Reasoned

- **Where:** `core/src/utilities/clock.cpp:59-61`. `AdvanceMs` guards its multiply and then routes
  into this unchecked add. Test and bench only.
- **Fix:** guard the add as the multiply is guarded.

## The WASM boundary and its generators

### BND-1

**A JavaScript exception thrown by a port's host unwinds the core's frames without restoring the
stack pointer, and the core corrupts its own statics.** medium · Confirmed (re-run)

- **Where:**
  - `shell/src/bridge/facade.ts:71-83` and `shell/src/bridge/worker.ts:71-77,153-157`, which report
    the exception as one failed call and keep using the module.
  - Two comments say the opposite: `bridge/runtime.cpp:33` and
    `bridge/resource_access/frame_store_access/opfs_spill_sink.cpp:47-50` both say a throwing EM_JS
    "aborts the module".
- **What breaks:**
  - With `-fno-exceptions` there is no `stackRestore`, so each throw leaks shadow stack downwards,
    into static data where the `Runtime` lives.
  - With a document host whose `read` throws, `ProjectManager.list` still answers at 725 throws.
    By 750 an ordinary call traps with `null function or function signature mismatch`.
  - With a motion host whose drain throws, the 62nd tick traps.
- **Reach:** the shipped hosts catch their own errors (`spill-host.ts`, `document-host.ts`). It takes
  an uncaught browser exception or a host bug.
- **Fix:**
  - Treat any exception out of `_sph_facade_call` as the end of the module, which is the shape
    `remote-core`'s `die` already has.
  - Correct the two comments.
  - Restoring the stack alone is not enough, because the C++ state is left half-mutated.

### BND-2

**Threaded build: two heap views go stale after another thread grows memory.**
medium · mechanism confirmed, trigger reasoned

- **Where:**
  - `Module.HEAPF64` in `bridge/resource_access/motion_sensor_access/browser_motion_sensor_access.cpp:45-47`;
  - `module.HEAPU8` in `shell/src/bridge/facade.ts:72-80` and `core.ts:133`.
- **What breaks:**
  - Emscripten refreshes only bare `HEAP*` references. On a stale view, `.set` past its end throws
    `RangeError` and `.subarray` silently clamps.
  - A stale motion view makes every drain throw (each throw also leaks stack, as in
    [BND-1](#bnd-1)).
  - A clamped facade result reads as "malformed response" for a call that already took effect.
- **Trigger:** OpenCV's pthreads backend allocates on pool threads, so a build followed by
  continued capture would cross it. Not run end to end.
- **Fix:** bare `HEAPF64` in the EM_JS. In TS, take the view from the memory's buffer on each call.

### BND-3

**The browser project store does not keep a title's bytes.** medium · Confirmed (re-run)

- **Where:** `bridge/resource_access/project_store_access/browser_project_store_access.cpp:48-64,111-125`
  (`UTF8ToString`, `lengthBytesUTF8`, `stringToUTF8`). The input arrives through
  `ProjectManager::Create`, which checks only `empty()`.
- **What breaks** (from `create`, then `list`):

  | Title sent | Browser `list` answers | Native `list` answers |
  | --- | --- | --- |
  | `61 00 62` | `61` | `61 00 62` |
  | `00` | empty, which `Create` refuses to make | `00` |
  | `61 62 F0` | `61 62 D2 9B` (heap bytes read past the NUL) | `61 62 F0` |
  | `F7 80 80 80` | `ED BB 80 00` | `F7 80 80 80` |

- **Why it is not caught:** the contract suite never runs the browser port. The shell's own titles
  are ASCII, so only a crafted payload reaches it.
- **Fix:**
  - `Create` refuses a title that is not UTF-8 or that holds control bytes, as `NameSays` does for
    filenames, and the port refuses the same.
  - `ReadDocument` trims to what `stringToUTF8` wrote.

### BND-4

**The generator accepts explicit enum values and mirrors them by index.** medium · Confirmed (re-run)

- **Where:** `tools/contract_gen.py:335` keeps the name and drops the value. C++ crosses the value
  (`:897`, `:934`); TS crosses the index (`:1026`, `:1052`).
- **What breaks:** for `enum class Level { Low = 1, High = 5 }`:
  - C++ `Low` arrives in TS as `'High'`, and C++ `High` arrives as `undefined`.
  - TS `'High'` arrives in C++ as `Low`.

  No boundary enum has explicit values today; the next one silently miswires.
- **Why it is not caught:** the tests check that the value is accepted, and check the C++ text,
  never both halves against each other.
- **Fix:** raise on any `= value` other than the member's own index (`Ok = 0` stays legal).

### BND-5

**A non-const reference parameter is accepted, mirrored as an input, and whatever the manager
writes into it never crosses back.** low · Confirmed (re-run). `tools/contract_gen.py:54-56`.
**Fix:** raise on it.

### BND-6

**An `@boundary`/`@facade` class declared `final`, or with a base, vanishes from the mirror and the
dispatch, and the generator exits 0.** low · Confirmed (re-run). `tools/contract_gen.py:197`.
**Fix:** raise when a pending `@boundary` is followed by a line `CLASS_RE` does not take.

### BND-7

**`float` is in the subset and is decoded as an unchecked narrowing.** low · Confirmed (re-run).
1e300 is undefined behaviour, and inf in practice, which then passes any `> 0` check. No boundary
type uses `float` yet. `tools/contract_gen.py:28,537`. **Fix:** drop `float`, or give it a checked
door like `GetInteger`.

### BND-8

**The TS wire strips a leading BOM and replaces lone surrogates, so those strings do not round
trip.** low · Confirmed. `shell/src/bridge/wire.ts:12,40-44,109-112`. **Fix:**
`new TextDecoder('utf-8', { ignoreBOM: true })`, and a test with a leading U+FEFF.

### BND-9

**`wire_kind()` is dead code, and its tests pin `int32_t` to a wire width the emitters do not use**
(they send f64). low · Confirmed. `tools/contract_gen.py:554-576`, `tools/test_codec_gen.py:24-44`.
**Fix:** delete it, and point the tests at what ships.

**Known, not re-reported:**

- The facade decodes enums with a `static_cast` into a `uint8_t` enum, so 256 aliases to 0. This
  was declined as out of scope on PR #113.
- The facade has no layout version (ADR 0074 and the roadmap record this as open).

## The TypeScript shell

### SH-1

**Two capture loops after the camera-recovery path.** medium · Confirmed (read)

- **Where:** `shell/src/main.ts:553-563`, `:657-665` and `:677-689`.
- **What breaks:**
  - The state is reached when the camera dies between `camera.open` and `beginSession`, which an
    e2e test sets up and then asserts that `#enable` is offered.
  - The page then runs a plan-less `pump`. Pressing `#enable` starts `pump(plan)`, and nothing
    stops the first loop: each `pump` keeps its `loopStopped` flag in its own closure.
  - Each frame, the old loop drains every sample first, so the session's loop gets none. No ring or
    arrow is ever drawn, and the reticle refreshes only on the 250 ms heartbeat. The reviewer's
    harness printed `callbacks:2, drains:[1,0]` and `rings drawn: 0`.
  - `pump`'s comment claims to be "the one place that can promise there is only ever one" loop. It
    hides `#new-capture` and `#resume`, but not the `#enable` that `offerTheCameraAgain` raises.
- **Fix:** a loop-generation ticket that each `step` checks.

### SH-2

**The camera keeps streaming after the core refuses `Begin` or `Resume` before its own `Open`.**
medium · Confirmed

- **Where:** `shell/src/main.ts:616-632` and `:635-668`. The refusal comes from
  `capture_session_manager.cpp:254-262` and `:435`.
- **What breaks:**
  - On iOS, declining motion makes `RequireMotion` refuse before `camera_.Open`, so `Close` never
    runs and `onCloseCamera` never fires.
  - Every offer is withdrawn, but the track the page opened stays live, and the camera indicator
    stays lit for the life of the tab. The harness printed `camera tracks: live` and
    `offers visible: []`.
  - The core's comment says `Open` "lights the indicator", which is false in the browser.
- **Fix:** stop the page's stream whenever a refusal leaves nothing to press. Correct the comment.

### SH-3

**A sensorless desktop starts a capture that can never capture.**
medium · adapter confirmed, page reasoned

- **Where:** `shell/src/access/motion.ts:169-183` and `:393-432`.
- **What breaks:**
  - `detect()` reports `GyroAccel` because the constructors exist.
  - `start()` returns `ok` once the sensor object is built. The sensor error comes later, and the
    fallback has no gate, so every drain is `ok([])`.
  - The page shows "capturing — N cells planned" with the reticle parked and no message, which is
    what ADR 0044's refusal exists to prevent.
  - `docs/06-roadmap.md:502` ("not reachable from the shipped page") and
    `shell/e2e/shell.spec.mjs:3156` say otherwise.
- **Fix:**
  - `start` resolves only on the first real reading or a deadline, and otherwise answers
    `SensorUnavailable`.
  - Correct the two sentences.

### SH-4

**A muted camera track can bank a frozen frame.** medium · Reasoned

- **Where:** `shell/src/main.ts:155-158`, `:1040-1057` and `:1394-1405`.
- **What breaks:**
  - Every liveness check reads `ended`. Nothing reads `muted` or listens for `mute`.
  - On an interrupted track (WebKit Split View, for example) the dwell can still fire, and five
    copies of a stale frame are filed under the current cell.
- **To settle:** a browser test that fakes `muted` and dispatches `mute` mid-session, and an iOS
  device check.

### SH-5

**`main.ts:1689` calls `void remote.flush()` with no catch.** low · Reasoned. Once the worker is
dead, every `pagehide` logs an unhandled rejection.

### SH-6

**`startFresh` creates the project before `begin` and does not delete it when `begin` refuses.**
low · Reasoned. Each refused attempt leaves a titled project with no session.

## Tooling, CI and the build

### TL-1

**Nothing stops a red `main` from deploying.** medium · Confirmed (read)

- **Where:** `.github/workflows/deploy.yml:8-12` and `:68-75`.
- **What breaks:**
  - Deploy runs on `push: main` on its own, with no dependency on CI. The branches API reports `main`
    as unprotected; repository rulesets were not checked.
  - Deploy itself runs only typecheck, the single-threaded wasm build, its size budget and
    `playwright test shell/e2e`.
  - A commit that fails native, ASan, contracting, the layer rules, contract staleness, vitest or
    the wasm accuracy run is still published.
- **Fix:**
  - Trigger deploy from `workflow_run` with `conclusion == 'success'`, or make it a job in `ci.yml`
    that needs the other five.
  - Require those checks on `main`.

### TL-2

**The layer checker sees one spelling of an include, and edges laundered through unlayered files
walk past it.** medium · Confirmed (read)

- **Where:** `tools/layer_check.py:80`: `INCLUDE_RE` requires whitespace and then `"`. Also `:113-114`,
  `:140-142` and `:189-191`.
- **What it misses:**
  - `#include <sphanorama/managers/…>`: contracts is an `-I` root, so the angle form works.
  - `#include"…"`, with no space.
  - An engine reaching a manager through an unlayered header: `core/src/detail/x.h`,
    `../../../../bridge/runtime.h`, or `core/test/support/*.h`.
- **Fix:** match both delimiters and optional whitespace. Resolve angle includes against the
  contracts root. Refuse includes of unlayered files from layered ones.

### TL-3

**A committed `test.only` turns the browser suite green on one test.** medium · Confirmed (read)

- **Where:** `playwright.config.mjs` has no `forbidOnly`.
- **What breaks:** with `CI=true` on Playwright 1.56.1, a passing `test.only` beside a failing test
  prints "1 passed" and exits 0. Vitest refuses the same thing under `CI`, so only Playwright is
  exposed, and it is the deploy's only browser gate.
- **Fix:** `forbidOnly: !!process.env.CI`.

### TL-4

**`ctest` passes when no tests exist, and the contracting job has no other guard.**
medium · Confirmed (read)

- **Where:** `.github/workflows/ci.yml:354` and `tools/gate.sh:96`; also `ci.yml:119,253` and
  `gate.sh:72,83`.
- **What breaks:** on an empty test directory, `ctest` prints "No tests were found!!!" and exits 0.
  The native and ASan jobs are covered indirectly by their accuracy floors; the contracting job is
  not.
- **Fix:** `--no-tests=error` on all six calls.

### TL-5

**Every action is pinned by a movable tag, and the build job holds Pages-deploy permissions.**
medium · Reasoned

- **What is exposed:**
  - Third-party `mymindstorm/setup-emsdk@v14`, which installs the compiler for the shipped wasm,
    and `astral-sh/setup-uv@v5` run in the job that produces `dist`.
  - Workflow-level `pages: write` and `id-token: write` reach that build job; only the deploy job
    needs them.
  - `ci.yml` declares no `permissions`, the checkout keeps its token, and uv's own version floats.
- **Fix:** pin every `uses:` to a full SHA, and scope permissions per job.

### TL-6

**The OpenCV and stb pins are cache variables without `FORCE`, so a bump is ignored by an existing
build tree.** medium · Confirmed (read)

- **Where:** `cmake/opencv.cmake:27-30` and `cmake/stb.cmake:14-15`. `build/native-debug/CMakeCache.txt`
  holds the current values.
- **What breaks:**
  - After a bump, a reconfigured tree keeps the cached SHA, and the rev-parse check compares
    against the cached value, not the file.
  - The local gate then goes green on the old OpenCV while CI builds the new one, and the accuracy
    numbers written down locally come from the wrong library.
  - For stb, the bump fails with a misleading message.
- **Fix:** a plain variable (or `FORCE`) for the pin, with a separate override option if one is
  wanted.

### TL-7

**googletest is pinned by tag only, shallow and unverified.** low · Confirmed

- **Where:** `core/test/CMakeLists.txt:5-9` (`GIT_TAG v1.15.2`).
- This contradicts the reasoning `opencv.cmake` gives for pinning by SHA, which uses googletest as
  its example.
- **Fix:** pin `b514bdc898e2951020cbdca1304b75f5950d1f59`.

### TL-8

**The dist freshness check misreads ordinary Playwright filters.** low · Confirmed

- **Where:** `tools/check_dist_fresh.mjs:80-106`.
- **What breaks:** with the threaded build missing, it accepts runs that do load
  `bridge/test/module.spec.mjs`, and two threaded tests skip silently. It accepts:
  - `file:line`;
  - an absolute path, or `./bridge/…`;
  - a different case, such as `Bridge`;
  - a bare flag that swallows the filter after it.
- **Why:** Playwright strips `:line`, matches case-insensitively and tests the absolute path; the
  checker does none of those.

### TL-9

**With `SPHANORAMA_CORE_PROFILE` set, the single-threaded core is never compared against the C++.**
low · Confirmed

- **Where:** `tools/check_dist_fresh.mjs:528-562`. Four bridge tests then load a stale
  `build/wasm-release` without complaint.

### TL-10

**The bundle's freshness ignores some of its inputs.** low · Confirmed

- **Where:** `tools/check_dist_fresh.mjs:503-509` and `:681-690`.
- **What breaks:**
  - Edits to `vite.config.mjs`, `tools/build_sw.mjs`, `tsconfig.json` or `package-lock.json` are
    accepted.
  - The check compares against the newest file in `dist`, which is always `sw.js`, so
    `npm run build:sw` alone hides a stale bundle.

### TL-11

**Both line-scanning C++ checkers read only `.h/.hpp/.cpp/.cc`.** low · Confirmed

- **Where:** `layer_check.py:53` and `no_browser_check.py:21`. An `.inl` or `.cxx` file holding
  `__EMSCRIPTEN__` or a manager include goes unreported.

### TL-12

**CI's accuracy step re-enables `set -e` before the acceptance count.** low · Confirmed

- **Where:** `ci.yml:143,158` and `:273,288`. A renamed-away `Acceptance` suite still turns the step
  red, but its explanation never prints.

### TL-13

**The size budget passes an empty profile.** low · Confirmed

- **What breaks:** it exits 0 with nothing measured. Only `.wasm` and `.js` are budgeted, while
  `stage_core` ships every `sphanorama-core.*` file.

### TL-14

**`setup_emsdk.sh` fetches and never checks out.** low · Reasoned

- **What breaks:** its `git fetch` line does nothing. The emsdk version `6.0.9` is written in three
  places (the script and both workflows), and nothing checks that they agree.

## Docs and ADRs

### DOC-1

**Six places still say "no composition root selects" the real registration engine. The browser
runtime selects it.** medium · Confirmed (read)

- **Where:**
  - `CLAUDE.md:47-48` ("the browser runtime still holds the null one"), which contradicts the same
    file's `:119`;
  - ADR 0052's banner (`:7-9`), whose Status line has no pointer to 0071;
  - `docs/06-roadmap.md:970-971` and `:58-59` ("nothing outside the tests calls [`Refine`] yet",
    though `PanoramaBuildManager` does);
  - `docs/03-architecture.md:446-447`;
  - `docs/05-toolchain-and-testing.md:37-38`;
  - `core/CMakeLists.txt:69-70` ("the module, which does not link it yet").
- **Truth:**
  - `bridge/runtime.h:124-129` composes `FeatureRegistrationEngine{frames_, FeatureDetector::Orb}`.
  - The module measures 700,054 B gzipped.
  - `shell/e2e/shell.spec.mjs:887` builds a preview.
- **Fix:** delete or correct each sentence, and extend ADR 0052's Status line with the pointer to
  0071.

### DOC-2

**docs/04 §4.2 describes a spill tier that dies on reload, a JPEG-encoded heap tier and a startup
probe. None exist, and the same document says the opposite twenty lines later.** medium · Reasoned

- **Where:** `docs/04-runtime-topology.md:38-43` and `:53-58`. The engineering skill's "the rest
  rest encoded or spilled" (`:349`) says the same.
- **Truth:**
  - The tier survives a reload: `spill-host.ts:144-219` reads its index back (ADR 0030, ADR 0063).
  - Resting frames are raw RGBA; `HeapEncoded` is only a label.
  - The ceiling is read from `navigator.deviceMemory`, and §4.2's own `:91-94` rules out a probe.
- **To settle:** `shell/e2e/shell.spec.mjs:3480` (a capture interrupted by a reload is offered back).
- **Fix:** replace `:53-58` with a pointer to §4.3 and the ADRs, and correct the table rows and the
  skill's sentence.

### DOC-3

**"The one image that leaves is a `FramePreview` … bounded by `kFramePreviewMaxEdge`" is false since
`PanoramaPreview` (ADR 0071).** medium · Confirmed (read)

- **Where:**
  - `contracts/README.md:50-54`;
  - `docs/03-architecture.md:181-182`;
  - `contracts/cpp/sphanorama/managers/capture_session_manager.h:203` and its generated mirror.
- **What breaks:** `kFramePreviewMaxEdge` is 256, and `PanoramaPreview` hands across up to 2048 wide.
  A check written from the README would refuse every panorama preview.
- **Fix:** name both images, with their bounds and ADRs 0038 and 0071, then regenerate the mirror.

### DOC-4

**"Resource access is implemented twice — TypeScript for the browser" is stated as the rule.**
medium · Confirmed

- **Where:**
  - the engineering skill, `:97-99`;
  - `contracts/README.md:62-64`;
  - `docs/04:199-201` ("three resource accesses are also excluded");
  - the skill's "the three resource accesses that move bytes through the shared heap".
- **Truth:**
  - The codec has one implementation, inside the core (ADR 0087), and so does the frame store.
  - Export is null in both builds, and compute has no implementation.
  - The browser ports are C++ in `bridge/`.
  - Four resource accesses are not mirrored, not three.
- **What breaks:** a contributor following the standard writes a TypeScript `IImageCodecAccess`,
  which ADR 0087 rejected.
- **Fix:** reword both statements to match docs/03 §3.5, and make the count four.

### DOC-5

**ADR supersession links are missing or one-sided.** low · Confirmed

- **Amendments with no pointer at either end:**
  - 0018 is amended by 0032 (0018:69 still says "`BurstSpec` is unchanged");
  - 0026 is amended by 0070 (0026:32 and :38).
- **Index rows missing an amendment their Status line carries** (`docs/adr/README.md`): 0002
  (0087), 0024 (0074), 0051 (0058), 0078 (0081) and 0081 (0083).
- **The reverse:** the 0052 and 0070 rows cite 0071, and their Status lines do not.
- **Stale line references:** `docs/adr/README.md:77-78` cites ADR 0052 `:61` and `:19-21`; quote the
  phrases instead.
- **A wrong path:** ADR 0063:48 gives `bridge/tier-claim.ts`; the file is
  `shell/src/bridge/tier-claim.ts`.

### DOC-6

**The README's build section is wrong in two ways.** low · Confirmed

- **Where:** `README.md:61` and `:67`.
- **What is wrong:**
  - "All six presets — three native, three WASM": there are seven, four of them native.
  - The list of what must be on `PATH` omits clang, which `native-contracting` and `gate.sh:93`
    need.

### DOC-7

**docs/05 names tooling that does not exist.** low · Confirmed

- **`docs/05:51-52`:** there is no workbox and no COOP/COEP shim. ADR 0011 rejected the shim, and
  the worker is a hand-written `shell/public/sw.js`.
- **The §5.4 table (`:193-196`)** describes:
  - Vitest against the real WASM module (none exists);
  - Testing Library (the dependency is happy-dom);
  - `--use-file-for-fake-video-capture` and replayed logs (neither is used);
  - a perf bench and trace budget (neither exists).
- **`docs/04:29` and `docs/05:27`** say `IComputeDeviceAccess` reports threads. Nothing implements
  it; threads come from `sph_probe_runtime`.

### DOC-8

**The list of CI checks claims to be complete and is not.** low · Confirmed

- **`docs/05` §5.3 (`:56-149`) and `docs/00:141-144`** omit the `contracting` job.
- **`docs/05:163`** says "twenty steps before"; it is 27.
- **`tools/gate.sh:5`** says "four CI jobs"; there are five.
- **`docs/05:60`** omits `bridge/` from the layer check's reach.

### DOC-9

**Stale planned-cell counts.** low · Confirmed

- **Where:** `docs/06-roadmap.md:72-75` ("44 planned cells rather than 32") predates ADR 0075.
- **Today's counts:** the page's spec plans 38 cells for 4:3 held sideways and 50 for 16:9; held
  upright, 32 and 64.

### DOC-10

**The roadmap says detector cost in the WASM build "has not been measured".** low · Confirmed

- **Where:** `docs/06-roadmap.md:1058-1062`.
- **Truth:** `:997-999` of the same file reports node timings of 0.16–0.94 s, and ADR 0071 chose ORB
  on them.

### DOC-11

**Smaller stale sentences.** low · Confirmed

- `docs/04:220-221` says the browser ports are "not built yet". They are built.
- `docs/03:443` and `docs/06:472` say the repository has no build stage. It has, since ADR 0070.
- The docs/03 §3.1 diagram:
  - lacks the quality engine's edge to the frame store;
  - draws two edges the build manager does not have;
  - omits `IEventBus`.
- The skill (`:225`) and `docs/00:180` list `clients/{capture,review}`. `clients/panorama` exists too.
- The skill (`:342`) says Playwright runs "in one worker". Nothing sets `workers`. (Reasoned.)
- `docs/06:60-61` says the kept lens is undecided. ADR 0067 decided it.
- `README.md:10-12` says stitching is not wired, while `:48-50` says a phone builds a preview.
- `docs/01:20-21` points to an "exposure volatility axis" that does not exist.

---

## Examined and found sound

What the reviewers read and checked and that held. An area missing from here was not reached.

**Engines**

- **Registration:** every bounds guard; pins released on every path; Kabsch, reflection repair,
  Shepperd and the rotation convention; every listed `Refine` refusal; the focal fit.
- **`RenderPreview`:** the frame index, the stride and row bounds, the sampling clamp, and the
  give-back on refusal.
- **Box preview and sharpness:** 64-bit sums and indexing.
- **Coverage planner and pose:** numerical guards, wraparound and termination.
- **The layer rule:** every engine is stateless and includes only what it may.

**CaptureSessionManager**

- **Ownership:** burst ownership, and every way out of a burst (unlock and cool).
- **Limits:** `Trim` against its cap, and `CandidatePreview`'s residency restore.
- **Refusal orders:** `Resume`'s, including the tier-generation refusal of another tab's document;
  `Begin`'s late `Clear`; and `FillGaps`'s and `ArmBurst`'s guards.
- **Dwell and motion:** the dwell's clamping; `OnMotion`'s clamps.
- **The codec:** enum ranges, trailing fields, the tier line and the counter.

**PanoramaBuildManager and ProjectManager**

- **`Start`:** every listed refusal, with the right code.
- **Stepping:** the step count and fraction.
- **Failure and cancellation:** failure, cancellation and a store cleared at each stage.
- **Identities:** frame and build ids across reloads and tabs.
- **Sentinels:** candidate 0, generation 0 and empty feature sets.
- **Agreement with the page:** the pick rule matches `shell/src/clients/review/candidates.ts`.
- **Also held:** gap judging, the layer rules, and `KeptWeight`, re-derived by hand.

**Resource access and utilities**

- **Frame store accounting:** every residency change goes through `Reclassify`, refusals leave the
  totals intact, and the skip-rewrite rule is reset on every path that can lose the sink copy.
- **wasm32 `size_t`:** bounded by the browser ceiling.
- **The frame store contract suite:** covers every promise except [RA-3](#ra-3).
- **`StbImageCodecAccess` and `gpano_xmp`:** hold.
- **`camera_model`:** the Jacobian, the Newton step and the fold check were re-derived.
- **`rotation_averaging`:** the 2.4e-6° fixed-point figure was re-measured and is real.
- **The rest:** `quaternion_average`, `kept_lens`, `equirect`, `arena`, `event_bus`, `config_store`,
  `logger`, `diagnostics` and the null ports.

**The boundary**

- **`wire.h`:** every read is bounded; `GetCount` divides; the integer doors are exact.
- **`codec.h`:** every `GetCount` minimum matches its field sums.
- **The facade:** all 27 cases decode before the `ok()` check; unknown ids answer `NotFound`.
- **TS against C++:** they agree field by field, including the u64/bigint fields; every identity
  stays below 2^53.
- **The browser ports:** their guards hold, `heap_budget`'s arithmetic holds, page↔worker
  transfers are fresh buffers, and the generator refuses most constructs it should.

**The shell**

- **Ordering:** worker reply order (fill then guidance; replan against coverage reads; `armBurst`
  against `onMotion`); the four coverage-refresh flags; the review panel's per-render ticket kept
  apart from its per-cell count.
- **The camera:** the lock-write chain; `armOnce`'s re-check; `remote-core`'s death and boot
  teardown; the camera error mapping.
- **Also held:** the service worker's install, activate and fetch policy, and COOP/COEP.

**Tooling and CI**

- **Parity:** `gate.sh` and `ci.yml` match step for step, and every test file is wired to a runner.
- **The other checkers:** the provenance, tracked-file, reading, conflict-marker, table and
  fused-build checkers and their suites hold.
- **Pins and secrets:** stb is SHA-256 verified; the npm and uv lockfiles carry integrity hashes;
  caches are per branch; nothing uses `pull_request_target` or a secret.

**Docs**

- **Every published accuracy figure re-measures as stated:** chain, solved and focal medians for all
  three detectors; composition 1.193; registered and built previews.
- **The WASM table re-measured,** and so did the module sizes (700,054 B and 730,729 B gzipped),
  the pose creep (1.906°), the planner counts and the rotation-solver tables.
- **References:** every ADR index link resolves, and every backticked path in the non-ADR docs
  exists, apart from those listed above.

## Not reviewed

- `tools/synth_dataset.py`'s rendering mathematics, and the vitest suites under the tooling lens.
- Most of `shell/e2e/shell.spec.mjs` (about 15 of 80 tests read); `spill-host`'s allocator
  arithmetic; `document-host`'s persist semantics; tier-claim under real Web Locks timing; the CSS.
- The threaded build end to end ([BND-2](#bnd-2)'s trigger).
- The tests-that-cannot-fail lens on the pose, coverage, composition and frame-preview tests, and on
  the manager tests.
- ADRs 0001–0045 claim by claim (checked for Status pointers only).
- These figures, not re-measured:
  - ADR 0087's encode timings;
  - the 0.6522 inlier fraction;
  - "2% out puts ORB at 0.50";
  - ADR 0080's 261/309;
  - ADR 0068's 2.84.
- Repository rulesets on GitHub, and what emsdk verifies by checksum.

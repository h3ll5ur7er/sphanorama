/**
 * Capture client entry point.
 *
 * Wires the browser adapters to the WASM core and renders the result. It contains no business
 * logic on purpose: reticle placement, acceptance and coverage are manager and engine decisions
 * behind contracts, and a client that computed them here would have to be unwound later.
 */
import type { RuntimeCapabilities, SphanoramaCore } from './bridge/core';
import { connectCore, type RemoteCore } from './bridge/remote-core';
import type {
  CapturePlan, CoverageState, NodeId, ProjectId, ProjectSummary, Quat,
} from '../../contracts/ts/contracts';
import { createCameraAccess } from './access/camera';
import { createMotionSensorAccess } from './access/motion';
import { flattenImuSamples, stopCameraStream } from './access/capture-host';
import { canvasDrawTarget, createFrameGrabber, GRAB_MAX_EDGE } from './access/preview-frame';
import { toImuSample } from './access/orientation';
import {
  describeFailure, describeGuidanceFailure, describeLocks, formatCapabilities,
} from './clients/capture/status';
import {
  canCapture, describeGuidance, reticleRadius, unwrapDegrees, RETICLE_LOCKED_RADIUS,
  RETICLE_MAX_RADIUS,
} from './clients/capture/guidance';
import { describeResumeRefusal, resumableProject } from './clients/capture/resume';
import { describeAttitude } from './clients/capture/attitude';
import { planOverlay } from './clients/capture/overlay';
import { createOverlayPainter } from './clients/capture/painter';
import {
  createReviewPanel, paintPreviewOnCanvas, type ReviewPanel,
} from './clients/review/panel';

const el = <T extends Element>(id: string) => document.getElementById(id) as unknown as T;

const viewfinder = el<HTMLVideoElement>('viewfinder');
const horizonGroup = el<SVGGElement>('horizon-group');
const reticle = el<SVGCircleElement>('reticle');
const cellLayer = el<HTMLElement>('cell-layer');
const targetArrow = el<HTMLElement>('target-arrow');

/*
 * The panel, and getting it out of the way of the picture.
 *
 * It is most of the screen, and the screen is where the markers are — so it folds itself once a
 * capture starts, and from then on it is the user's: nothing reopens it but a press. The state it
 * carries is one tap away; what it must not be is parked over the viewfinder for the whole of the
 * part of this app you have to aim with.
 *
 * The label names the press, not the state, so an open panel offers "hide" — while `aria-expanded`
 * names the state, because that is what it means. They read as contradicting each other and do
 * not.
 */
const panel = el<HTMLElement>('panel');
const panelToggle = el<HTMLButtonElement>('panel-toggle');
function openPanel(open: boolean) {
  panel.dataset.open = String(open);
  panelToggle.setAttribute('aria-expanded', String(open));
  panelToggle.textContent = open ? 'hide' : 'details';
}
openPanel(true);
panelToggle.addEventListener('click', () => openPanel(panel.dataset.open !== 'true'));

const stage = el('stage');
const coreCaps = el('core-caps');
const cameraState = el('camera-state');
const motionState = el('motion-state');
const orientationOut = el('orientation');
const guidanceOut = el('guidance');
const locksOut = el('locks');
const facadeOut = el('facade');
// Written once, at load, and never touched again: it describes the bundle rather than the session,
// and a line that can change is a line a screenshot cannot be trusted on. `define` replaces the
// identifier at build time (see vite.config.mjs); the fallback is for `vite dev`, which does not.
el('build').textContent =
  typeof __SPHANORAMA_BUILD__ === 'string' ? __SPHANORAMA_BUILD__ : 'unknown';
const enableButton = el<HTMLButtonElement>('enable');
const captureButton = el<HTMLButtonElement>('capture');
// The two halves of coming back to a capture: the offer, shown at load when the listing says
// there is a session to resume, and the way out when that resume is refused (ADR 0036).
const resumeButton = el<HTMLButtonElement>('resume');
const newCaptureButton = el<HTMLButtonElement>('new-capture');
const reviewElements = {
  panel: el<HTMLElement>('review'),
  map: el<HTMLElement>('coverage-map'),
  stripHeading: el<HTMLElement>('strip-heading'),
  strip: el<HTMLElement>('strip'),
};

const camera = createCameraAccess(navigator.mediaDevices);
// One canvas for the session: grabbing a frame means drawing the viewfinder into it and reading
// the pixels back, which is the only route from a <video> to bytes that works on every browser
// we ship to (ADR 0021).
const grabFrame = createFrameGrabber(canvasDrawTarget(document.createElement('canvas')));
const motion = createMotionSensorAccess(window);

// The stream the page opened. It stays on this side because a MediaStream cannot cross to the
// worker the core runs in, so the host asks and the page stops (ADR 0019).
let cameraStream: MediaStream | null = null;

/**
 * Whether there is a camera in hand *now*.
 *
 * Asked of the tracks rather than of the reference, because a `MediaStream` whose tracks have all
 * ended is still a perfectly good `MediaStream` object. `cameraStream !== null` was the page's
 * test for this and it answers a different question — "was one ever opened?" — which is the same
 * shape of mistake as reading a capability where a sample was wanted.
 *
 * It also does not depend on the `ended` event having been delivered. The listener below is what
 * *reports* the loss; this is what *knows* it, so a track that was already dead when `open`
 * returned is caught too, and the two cannot fall out of step because there is only one fact.
 */
function cameraHeld(): boolean {
  return cameraStream !== null
    && cameraStream.getTracks().some((track) => track.readyState === 'live');
}

/**
 * Whether the camera went away *without* the core asking for it.
 *
 * Not the same fact as `!cameraHeld()`, which is also true after an orderly `End` — the core
 * closes the camera on its way out, and a session that has finished is not a session that lost
 * its camera. The capture loop needs the narrower one: an unexpected loss has to stop it, while a
 * closed session must fall through to the ordinary failing-tick path, which already says the right
 * thing and clears the right things. Conflating them made a session end announce "the camera was
 * taken away" and swallow the guidance failure, which an existing browser test caught.
 *
 * The reason is recorded because nothing else records it; the *state* is still read off the tracks.
 */
let cameraTakenAway = false;

// Every write to the camera's lock state, in the order it was asked for.
//
// `applyConstraints` takes as long as it takes, and three callers write here: an arm, the release
// after a refused arm, and the core's own `onReleaseLocks` at the end of a burst. None of them
// awaited each other, so a release issued for burst 1 could land *after* burst 2 had read the
// state back and told the core three locks were held — measured landing 80 ms into a burst the
// core believed was locked. ADR 0022's read-back cannot catch that, because the write it would
// have to see happens after the read.
//
// A chain rather than a lock: the writes are all short, order is the only thing that matters, and
// a failed one must not stop the next (a camera that refuses a constraint is a supported outcome,
// and the release after it is exactly when the ordering matters most). Module scope because the
// chain has to span a session — the core's release for the last burst of one session can still be
// in flight when the next session applies its first locks.
// The last thing `#locks` said about a burst, so a release can say the locks are gone without
// throwing away *which* locks they were. The row's whole job is to answer "was the camera free to
// re-expose between these frames?", and a bare "released" answers it for nobody.
let lastLocksLine = '';
/**
 * What a caller of `writeLocks` gets back: the camera's answer, or the fact that there was not one.
 *
 * The two used to be the same value. A write that timed out was manufactured into
 * `{ok: false, CameraUnavailable}` — the same shape a camera that *says no* produces — and every
 * caller downstream then treated "the track has not answered" as "the track refused". They are not
 * the same fact and the difference is the whole of ADR 0022: a refusal is a known state of the
 * camera, and a timeout is no state at all. Firing a burst over the second is firing over an
 * exposure that may change halfway through it, which is exactly what locks exist to prevent.
 */
type LockWrite =
  | { answered: true; done: Awaited<ReturnType<typeof camera.setLocks>> }
  | { answered: false };

// Painted from what the write *resolved with*, not from having queued it.
//
// Both callers fire and forget, so saying "released" on the next line asserted a state of the track
// the track had not reached — and with a bounded chain it might reach it three seconds later, or
// not at all. Waiting for the answer costs nothing here (the burst is over) and makes the row true
// rather than intended, which is the one thing this row is for.
//
// `had` is passed in rather than read off `lastLocksLine` here, because by the time this runs the
// variable may have been cleared by something else. `End()` disarms and *then* closes the camera,
// so an ordinary session end with a burst in flight queued a release, ran `onCloseCamera`'s clear,
// and painted the row as "no burst has run yet" about a burst that had just released its locks.
// The empty string means two things — nothing has run, and the record was discarded — and only the
// caller knows which one it was holding when it asked.
function showLocksReleased(row: Element, write: LockWrite, had: string) {
  if (had === '') {
    // Nothing to be a release *of*. Saying "released" here would be the row's only line about a
    // burst that never ran.
    row.textContent = 'no burst has run yet';
    return;
  }
  if (!write.answered) {
    // Not "released", because nothing has said so. The write is still on the chain and will land
    // when the track gets to it; what this row must not do is report an outcome it has not seen.
    row.textContent = `${had} · release not answered`;
    return;
  }
  const done = write.done;
  row.textContent = done.ok
    ? `${had} · released`
    : `${had} · release refused — ${done.status.detail || done.status.code}`;
}
let lockWrites: Promise<unknown> = Promise.resolve();
// How long a single lock write may take before the chain gives up waiting for it.
//
// `applyConstraints` is a promise the platform owns, and a camera that never settles one is not
// hypothetical — a track pulled away mid-call resolves nothing. Without a bound the chain parks
// for the life of the tab: every later write queues behind it, and because `armAt` awaits its own
// write, `arming` stays true and no burst can ever be armed again. A slow camera merely arrives
// late; this is what stops a stuck one from being permanent.
const LOCK_WRITE_TIMEOUT_MS = 3000;
function writeLocks(
  wanted: { exposure: boolean; whiteBalance: boolean; focus: boolean },
): Promise<LockWrite> {
  // The *caller* stops waiting after the timeout; the chain does not. Those are different things
  // and conflating them is worse than having no timeout at all: if the queue advanced on the race,
  // a stuck write would be overtaken by the release queued behind it, the release would reach the
  // track first, and the stuck one would land afterwards — ending a session with the camera locked,
  // which is the failure the ordering exists to prevent.
  //
  // So the chain is built from the real promise and only the answer is raced. A stuck write still
  // delays everything behind it, and that is correct: a camera that has not answered has not
  // answered, and guessing the order it will finish in is what produced the bug above.
  //
  // The clock starts when this write reaches the track, not when it joins the queue. Started at
  // enqueue, the budget was spent waiting for the writes in front — and each refusal queued a
  // release of its own behind the write it had given up on, so the queue grew by one write per
  // failure. Measured on a camera taking 700 ms per constraint: the first burst succeeded, the
  // second was refused after 3007 ms of which 2463 were spent in the queue, and every burst after
  // it failed the same way at 3 s intervals while the track answered fourteen constraints back to
  // back without an idle moment. One cell per session, blamed on a camera that was answering
  // everything it was asked.
  let reached: () => void;
  const reaches = new Promise<void>((resolve) => { reached = resolve; });
  const settled = lockWrites.then(() => {
    reached();
    return camera.setLocks(wanted);
  });
  lockWrites = settled.catch(() => undefined);
  return Promise.race([
    settled.then((done): LockWrite => ({ answered: true, done })),
    reaches.then(() => new Promise<LockWrite>((resolve) => {
      setTimeout(() => resolve({ answered: false }), LOCK_WRITE_TIMEOUT_MS);
    })),
  ]);
}

/** The page's end of the worker: what it pushes across, and the one thing the worker asks back. */
let remote: RemoteCore;

/**
 * Arms a burst at a cell, filled in by the capture loop while a session is running.
 *
 * A function rather than a call into the core, because arming has to happen where the loop can
 * see it (ADR 0018): the burst's first frame arrives on the next tick, and the loop is what has
 * to have a frame waiting by then.
 */
let captureCell: ((node: NodeId) => Promise<boolean>) | null = null;
/**
 * The cell guidance last pointed at — what the capture button captures.
 *
 * Null until guidance has named one, rather than a placeholder id: there is no cell zero to fall
 * back on, and arming against a number nothing chose would fail with NotFound for a reason the
 * user could do nothing about.
 */
let targetNode: NodeId | null = null;
/**
 * Which locks this camera says it can take, from `getCapabilities` at open time.
 *
 * Kept because arming asks for exactly these and no more: a lock the track has no manual mode
 * for is one `applyConstraints` will not give, and asking anyway costs the burst (ADR 0022).
 */
let lensLocks = {
  supportsExposureLock: false, supportsWhiteBalanceLock: false, supportsFocusLock: false,
};

/**
 * Keeps the motion readout showing what is actually feeding the core.
 *
 * The capability is the core's vocabulary and the source is the browser's. Both matter, and only
 * together do they explain a phone whose horizon looks wrong: a quaternion sensor that could not
 * start hands over to the Euler event asynchronously, and the two behave differently in exactly
 * the pose this app spends its time in (ADR 0017).
 */
let motionCapabilityShown = 'unknown';
function reportMotionSource(capability?: string, lostReason = '') {
  if (capability !== undefined) motionCapabilityShown = capability;
  const source = motion.source();
  motionState.textContent =
    `${motionCapabilityShown} · ${source}` + (lostReason ? ` · ${lostReason}` : '');
}

/**
 * Starts the worker the core runs in and connects to it (ADR 0019).
 *
 * The module URL is resolved here because the page is the side that knows the base path, and the
 * module is fetched at runtime rather than bundled: it is an artifact of the C++ build, and which
 * of the two builds (ADR 0011) is present is decided by what the deploy copied in.
 */
async function startCore() {
  const worker = new Worker(new URL('./bridge/worker.ts', import.meta.url), { type: 'module' });
  return connectCore(worker, `${new URL(import.meta.env.BASE_URL, location.href).href}core/sphanorama-core.js`);
}

function renderCapabilities(capabilities: RuntimeCapabilities, canSpill: boolean) {
  coreCaps.textContent = formatCapabilities(capabilities, canSpill);
}

/**
 * Whether motion was running when this session was enabled.
 *
 * Held here because a resume that is refused hands the decision back to the user, and the press
 * that answers it — "start a new capture" — arrives long after `enable` has returned. Asking the
 * adapter again at that point would be a second permission story to tell; this is the answer the
 * gesture already got.
 */
let motionIsRunning = false;


/**
 * Enabling has to happen inside a user gesture: iOS rejects the motion permission request
 * otherwise, and does so in a way indistinguishable from a decline.
 *
 * `resume` names the project to pick back up, or is null for a new sphere. Both take this same
 * path: a resume needs the camera and the sensor exactly as a fresh capture does, and the only
 * thing that differs is which call to the session manager comes at the end of it.
 */
async function enable(core: SphanoramaCore, resume: ProjectId | null) {
  enableButton.disabled = true;
  resumeButton.disabled = true;

  // Asked for, because a camera that is not asked answers with the browser's default rather than
  // its own best — 640x480 in Chromium, a quarter of the pixels the grabber's cap already budgets
  // for. So the ask is that cap: the frame the core stores is the frame the camera was opened to
  // produce, and when the cap moves the ask moves with it.
  //
  // The shape is asked for too, and 4:3 rather than the 16:9 a bare long edge gets handed. On a
  // phone that is not a preference between two crops: the sensor is 4:3 and the widescreen video
  // mode is made by throwing away the top and bottom of it, so asking for the taller frame asks
  // for more of the picture. What it buys is cells. Vertical field of view is what sets the ring
  // count, and 66 degrees across at 16:9 is 40 degrees tall against 52 at 4:3 — measured through
  // the whole app, 44 cells planned rather than 32, a third more of the sphere to shoot for a
  // frame that sees less of it.
  // Started before the camera is awaited, and awaited after it. iOS grants motion only during a
  // transient user activation, and the camera prompt is exactly the kind of await that spends
  // one — so asking afterwards is asking a gesture that has already ended, which iOS rejects
  // unread. That is an iPhone reporting `motion unavailable` in every orientation forever, with
  // no way to aim at a cell. The adapter was already careful not to spend the activation between
  // its own two requests; this is the same care one level out.
  const startingMotion = motion.start(60);

  const opened = await camera.open({
    preferRearCamera: true,
    preferredWidth: GRAB_MAX_EDGE,
    preferredHeight: Math.round((GRAB_MAX_EDGE * 3) / 4),
  });
  if (opened.ok) {
    // Pushed before the core is asked to begin: the plan is sized from the lens, and the core
    // reads the lens through a synchronous port that cannot wait for getUserMedia — nor for a
    // message still in flight.
    remote.setCamera(opened.value);
    lensLocks = opened.value;
    // Held here so the core can ask for it to be stopped: Close is a synchronous port call and
    // cannot reach a MediaStream itself.
    const stream = camera.stream();
    cameraStream = stream;
    // A fresh camera is not a lost one, whatever the last one did.
    cameraTakenAway = false;
    // A camera can also go away without anyone asking. Permission revoked from the browser's own
    // UI, another app taking the lens, a phone call: the track ends, and until this existed the
    // page never learned. `onCloseCamera` is the *core's* route and it was the only one, so what
    // was left behind was a non-null `MediaStream` with every track dead — which the resume button
    // reads as "a camera is in hand" and begins a session against, and a `#locks` row still
    // quoting a track that no longer exists.
    //
    // Guarded on identity rather than run unconditionally, because a stream can outlive its turn:
    // `camera.open` stops the previous tracks itself, so the old stream's `ended` arrives *after*
    // a new one is in hand, and clearing then would throw away the live camera on the strength of
    // the dead one's news.
    // What *reports* the loss. What *knows* it is `cameraHeld`, which reads the tracks — so this
    // does not clear `cameraStream`, and clearing it would be unobservable: every reader now asks
    // the tracks, and a reference to a stream of dead tracks answers the same as no reference at
    // all. A reviewer proved the point from the other side, by deleting the clears this used to do
    // and finding the whole suite still green.
    //
    // It does not clear `lastLocksLine` either, and that needs its own reason rather than the one
    // above — nothing derives that string from the tracks. The reason is not "no reader is left",
    // which was the first answer and is wrong: an arm already in flight when the loop stops reaches
    // `unlock()` afterwards, and `unlock` reads this line. The reason is that what it would read is
    // the right thing to read — a release issued for a camera that has just been taken away *is* a
    // release of the locks that camera was holding, and naming them is more use than naming
    // nothing. The core's own route clears it because there another session can follow and would
    // inherit the line.
    //
    // `stopCameraStream` is not bookkeeping and stays: a stream can lose one track and keep another
    // lit, and the camera indicator staying on is its own bug report.
    const forgetCamera = () => {
      if (stream === null || cameraStream !== stream) return;
      stopCameraStream(stream);
      cameraTakenAway = true;
      cameraState.textContent = 'taken away';
    };
    for (const track of stream?.getTracks() ?? []) track.addEventListener('ended', forgetCamera);
    cameraState.textContent = `${opened.value.maxWidth}×${opened.value.maxHeight}`;
    viewfinder.srcObject = camera.stream();
  } else {
    remote.setCamera(null);
    lensLocks = {
      supportsExposureLock: false, supportsWhiteBalanceLock: false, supportsFocusLock: false,
    };
    cameraState.textContent = 'unavailable';
    stage.textContent = describeFailure(opened.status);
  }

  const capability = await motion.capabilities();
  if (capability.ok) remote.setMotion(capability.value);
  const started = await startingMotion;
  if (started.ok) {
    reportMotionSource(capability.ok ? capability.value : 'unknown');
  } else {
    // The core is told None, and that is a supported configuration rather than a failure: the
    // manager puts PoseEngine into vision-only mode and no other component learns the difference
    // (docs/03 UC-4). Declining motion on iOS is the common way to land here.
    remote.setMotion('None');
    // With the reason, not just the word. A declined grant, a gesture that had already expired
    // and a device with no sensors at all printed the same thing here, and the status that tells
    // them apart was being dropped on the floor — which is what made an iPhone reading
    // unreadable. The row that reports a *running* source has carried its reason since ADR 0025;
    // this is the branch that did not.
    motionState.textContent =
      `unavailable · ${started.status.detail || started.status.code}`;
    // The stage line is deliberately left alone. It used to carry this failure too, which was
    // wrong twice: `describeFailure` tells the user to change a permission and reload, and there
    // is nothing to fix — capture without motion is a supported configuration the very next line
    // goes on to start. It was also invisible, because every path through `beginSession` writes
    // the stage, and `beginSession` runs whenever the camera opened, which is the only case this
    // branch wrote it in at all. The reason belongs on the motion row, and is on it.
  }

  enableButton.hidden = true;
  resumeButton.hidden = true;
  motionIsRunning = started.ok;
  // The camera is what a session needs; motion only makes aiming easier. Refusing to capture
  // without it would turn a supported degraded mode into a dead end.
  //
  // Asked of the stream rather than of `opened.ok`, because they answer different questions.
  // `opened.ok` is a fact about a call that has already returned; two awaits sit between it and
  // here — the motion capability and the sensor start — and a track that ends inside that window
  // runs `forgetCamera` and nulls the stream. The page then began a session against a camera
  // nobody was holding: "capturing — 32 cells planned", the panel folded, the shutter enabled,
  // and `#camera-state` saying the camera had been taken away, all on screen at once.
  const stillHeld = opened.ok && cameraHeld();
  if (opened.ok && !stillHeld) {
    stage.textContent = 'the camera was taken away before the capture could start';
  }
  if (stillHeld) await beginSession(core, started.ok, resume);
  // Still pumped, so the sensor readout stays live and the reason stays on screen — the same
  // thing a camera that never opened gets, because from here it is the same situation.
  else if (started.ok) pump(core, null, true, null);
}

/**
 * Opens a project and a session, then hands the plan to the render loop.
 *
 * A project comes first because a session belongs to one — the manager writes the session's
 * documents through the project store, and a capture with nowhere to be saved is a demo.
 *
 * With `resume` set the project already exists and so does its session: the manager reads what it
 * wrote, replans from the spec and lens the document carries, and hands the frames it names back
 * to the store (ADR 0029). Nothing is created, and in particular no `begin` — that would empty
 * the spill tier holding the very frames the resume is reaching for (ADR 0034).
 */
async function beginSession(core: SphanoramaCore, motionRunning: boolean,
                            resume: ProjectId | null) {
  const project = resume === null
    ? await startFresh(core, motionRunning)
    : await pickUp(core, resume);
  if (project === null) return;

  const plan = await core.captureSession.getPlan();
  if (!plan.ok) {
    stage.textContent = describeFailure(plan.status);
    pump(core, null, motionRunning, null);
    return;
  }

  const opening = resume === null ? 'capturing' : 'resumed';
  stage.textContent = motionRunning
    ? `${opening} — ${plan.value.nodes.length} cells planned`
    : `${opening} without motion — ${plan.value.nodes.length} cells planned, aim by hand`;
  // Folded once the session is under way. From here the picture is the interface, and the panel
  // was covering nearly two thirds of it.
  openPanel(false);
  pump(core, plan.value, motionRunning, project);
}

/**
 * Picks a capture back up, or says why it could not and leaves a way forward.
 *
 * The refusals are real ones — a document this build cannot read, a plan the stored spec no
 * longer produces, frames the tier lost — and every one of them is a state the user can still do
 * something from. So the reason goes on the stage line and the fresh-start button appears; what
 * must not happen is a page that offered a resume, failed it, and left nothing to press.
 *
 * The loop is deliberately not started here. It is started once, by whichever call settles the
 * session, and a `pump` on this path would leave a second one running under the capture the user
 * is about to start.
 */
async function pickUp(core: SphanoramaCore, project: ProjectId): Promise<ProjectId | null> {
  const resumed = await core.captureSession.resume(project);
  if (resumed.ok) return project;
  const refusal = describeResumeRefusal(resumed.status);
  stage.textContent = refusal.message;
  // Put back up, or not, by the same judgement that wrote the sentence — a button that disagrees
  // with the line above it is worse than either. A refusal another attempt could clear leaves the
  // offer pressable; one only a new build can change takes it away, so the user is not invited to
  // press a thing that will fail identically every time (ADR 0039).
  resumeButton.hidden = !refusal.offerAgain;
  newCaptureButton.hidden = false;
  return null;
}

/** A new project and a new session on it: the path every capture took before resume existed. */
async function startFresh(core: SphanoramaCore,
                          motionRunning: boolean): Promise<ProjectId | null> {
  const created = await core.project.create(`sphere ${new Date().toISOString().slice(0, 16)}`);
  if (!created.ok) {
    stage.textContent = describeFailure(created.status);
    return null;
  }

  const begun = await core.captureSession.begin(created.value as ProjectId, {
    strategy: 'Rings',
    // Zero means "probe the camera": the manager asks the camera port rather than being told,
    // and a number invented here would silently override it. What the port reports is a real
    // resolution and an assumed angle — see deriveFieldOfView in access/capture-host.ts.
    horizontalFovDeg: 0,
    verticalFovDeg: 0,
    overlapTarget: 0.3,
    acceptanceConeDeg: 4,
    coverPoles: true,
    // Replaced by whatever the sensor port reports. Sending None is the honest default: the
    // client does not get to promise the core a gyroscope.
    motion: 'None',
  });
  if (!begun.ok) {
    stage.textContent = describeFailure(begun.status);
    pump(core, null, motionRunning, null);
    return null;
  }
  return created.value as ProjectId;
}

/**
 * The capture loop: drain the sensor, hand the samples to the manager, render what it says back.
 *
 * No pose estimation and no reticle placement happen here — both come back from the core. With
 * no session the loop still runs, so the sensor readout stays live and the reason capture did
 * not start remains on screen.
 */
function pump(core: SphanoramaCore, plan: CapturePlan | null, motionRunning: boolean,
              project: ProjectId | null) {
  // The one place a render loop starts, and therefore the one place that can promise there is
  // only ever one. The way out of a refused resume has to disappear the moment something is
  // running, or a second press starts a second loop over the same session — two sets of
  // `requestAnimationFrame` callbacks drawing the same overlay and draining the same sensor.
  // Enforced here rather than at each caller because there are four of them and the invariant is
  // about the loop, not about any one path into it.
  newCaptureButton.hidden = true;
  // And the resume offer with it, for exactly the same reason: a refused resume can put that
  // button back (ADR 0039), and a capture started from the button beside it would otherwise leave
  // a live offer to start a second loop over the session already running.
  resumeButton.hidden = true;

  const cones = new Map((plan?.nodes ?? []).map((node) => [node.id as number, node.acceptanceConeDeg]));
  // Read once: coverage only moves when a cell is captured, and a facade round trip per frame
  // for a number that cannot have changed is the kind of waste that shows up as a hot phone.
  let nodesSatisfied = 0;
  // The last attitude the sensor reported, held between ticks because a tick with no samples has
  // nothing newer. Not the pose the core fused — no contract hands that back — so during a sensor
  // gap the markers hold still while the reticle, sized from the core's own answer, keeps moving.
  // The two agree whenever samples are arriving, which is whenever anyone is capturing.
  let attitude: Quat | null = null;
  // The coverage the map is drawn from, reused for the markers so the two renderings of one sphere
  // cannot disagree about which cells are done.
  let lastCoverage: CoverageState | null = null;
  const overlay = createOverlayPainter(cellLayer, targetArrow);
  // Both the guidance tick and the coverage refresh repaint, because either can change what the
  // markers should say and they do not happen together. A cell finishing updates coverage
  // *asynchronously*, after the tick that reported it has already drawn — and the tick that would
  // have redrawn may never come, since the loop only asks for guidance when a sample arrives. A
  // phone held still through the end of a burst would have watched the map fill in while the ring
  // for that very cell stayed empty.
  // Set when a tick failed and cleared when one succeeds. The markers describe where the cells are
  // relative to a pose, and a failed tick produced none — so nothing may paint them until guidance
  // works again, including the asynchronous coverage read that failure itself starts.
  //
  // It stops the *markers*, not the map: `paintOverlay` is its only reader, and `refreshCoverage`
  // goes on calling `review.show(...)`, which rebuilds every dot in `#review-map`. That is not an
  // oversight to guard — the failing branch calls `refreshCoverage` precisely so the map catches
  // the cell a failed tick may have banked, so the map repainting is the point. The line above
  // used to say "nothing may paint them", which is true of the overlay and was read as covering
  // both.
  let guidanceFailed = false;
  const paintOverlay = () => {
    if (guidanceFailed) return;
    if (plan === null || attitude === null || lastCoverage === null || targetNode === null) return;
    overlay.show(planOverlay({
      plan, coverage: lastCoverage, attitude, targetNode,
      // Measured every paint rather than latched. The video reports no size until the first frame
      // decodes, the box changes shape when the phone is turned, and a marker drawn against the
      // last orientation's box is a marker in the wrong place.
      fit: {
        frameWidth: viewfinder.videoWidth, frameHeight: viewfinder.videoHeight,
        boxWidth: cellLayer.clientWidth, boxHeight: cellLayer.clientHeight,
      },
    }));
  };
  const nodesTotal = plan?.nodes.length ?? 0;

  // The review panel only exists once there is a plan to map and a project to record a choice
  // against. Both arrive together or not at all.
  const review: ReviewPanel | null = plan === null || project === null ? null : createReviewPanel(
    reviewElements,
    {
      candidates: (node) => core.captureSession.candidates(node),
      candidatePreview: (node, candidate, maxEdge) =>
        core.captureSession.candidatePreview(node, candidate, maxEdge),
      setSelection: (node, candidate) => core.project.setSelection(project, node, candidate),
      selection: (node) => core.project.getSelection(project, node),
    },
    paintPreviewOnCanvas);

  /**
   * Re-reads coverage and redraws the map.
   *
   * Only when a cell completes, which is the only moment coverage can have changed — asking per
   * frame would be a facade round trip for an answer that cannot have moved, which is the same
   * reasoning the guidance line already follows.
   */
  const refreshCoverage = async () => {
    if (review === null || plan === null) return;
    const state = await core.captureSession.coverage();
    if (!state.ok) return;
    nodesSatisfied = state.value.nodesSatisfied;
    lastCoverage = state.value;
    review.show(plan, state.value);
    paintOverlay();
  };

  // Once at the start, and not only when a cell completes. Coverage can only *change* when one
  // does, which is why the refresh below is where it is — but a map that is drawn only on change
  // is empty at the moment it is most worth reading, which is before the capture, when it is what
  // tells you where to point.
  void refreshCoverage();
  let guidedOnce = false;
  // When guidance last answered, so a stream that goes quiet is noticed by the clock rather than
  // by a flag about the sensor's *capability*. See the heartbeat in `step`.
  let lastGuidedMs = 0;
  // Whether the last answer said a burst was still filling. A burst advances on this tick and
  // nothing else (ADR 0018), so it has to keep running even when the sensor has gone quiet.
  let firing = false;
  // Set the moment a burst is armed and cleared when guidance stops saying Firing. It exists
  // because of the hole ADR 0018 named: the tick right after arming has no guidance yet, so
  // `firing` is still false on the one tick that most needs a frame waiting for it.
  let armed = false;
  // Whether an arm is part-way through: the locks may be applied and the core may not have
  // answered yet. `armed` cannot serve — it is set only once the core says yes, and the whole
  // window this closes is the one before it does.
  let arming = false;
  // When the per-tick guidance line is allowed to overwrite an arming message.
  //
  // `#guidance` is rewritten by `describeGuidance` on every tick that carries samples, which on a
  // phone in a hand is every animation frame — so an arming failure written into it survived a
  // measured **four milliseconds**. Nothing owned "a message that has to outlive a frame":
  // `#stage` and `#locks` both do, but `#stage` carries the session line the browser suite asserts
  // on. A deadline the pump respects is the smaller change, and it makes every message `armAt`
  // writes readable — the refusals, and `capturing without … lock`, which is a real quality cost
  // (ADR 0031) that has been announced for one frame since it was written.
  let guidanceHeldUntilMs = 0;
  const sayForAWhile = (text: string) => {
    guidanceOut.textContent = text;
    // Long enough to read a short line, short enough not to sit over a reticle the user is
    // actively aiming with: at four seconds the aiming line was measured gone for 3.8 s while the
    // target moved through three cells, which trades one unreadable message for another.
    guidanceHeldUntilMs = performance.now() + 1200;
  };
  // Accumulated rather than taken fresh each frame, so rolling past the ±180 seam turns the
  // horizon by the two degrees the hand moved and not by the 358 the number jumped.
  let horizonDeg = 0;

  /**
   * Arms a burst at the cell guidance is pointing at, from inside the loop.
   *
   * Routed through here rather than called from an event handler, which is what ADR 0018 asked
   * for and did not have a caller to do: a burst advances on a tick and nothing else, so arming
   * somewhere the loop cannot see it leaves the first frame waiting for a tick that may not come.
   */
  const armAt = async (node: NodeId) => {
    // One arm at a time, refused before a single constraint is applied.
    //
    // Nothing serialised this. The click handler does not disable the button — only the pump does,
    // and it cannot until the core has set `firing_`, which is a worker round trip plus however
    // long `applyConstraints` takes: measured at 38 ms with an instant camera and 396 ms at
    // 120 ms per constraint. Inside that window a second tap applied the locks again, was refused
    // by the core with "a burst is already in flight", and then released the locks of the burst
    // that *was* running — a plain double tap re-metering a live burst, which is the exact failure
    // ADR 0022 exists to prevent and is undetectable in the frames afterwards.
    //
    // Refusing here rather than only unlocking more carefully, because applying the locks at all
    // while a burst holds them is the mistake; there is nothing this call could do with them.
    if (arming || armed || firing) return false;
    arming = true;
    try {
      return await armOnce(node);
    } finally {
      arming = false;
    }
  };

  const armOnce = async (node: NodeId) => {
    if (cameraLost()) {
      // Reachable without the loop: the end-to-end hook calls `captureCell` straight, and so did
      // the button before the loop learned to stop. A burst armed here would take five copies of
      // the `<video>`'s frozen last frame, which passes both of the grabber's guards.
      sayForAWhile('the camera was taken away — not capturing');
      return false;
    }
    // Applied and confirmed *before* arming, which is the whole ordering requirement (ADR 0022):
    // the burst's first frame arrives on the very next tick, and the core reads the lock state
    // through a synchronous port that cannot wait for applyConstraints.
    //
    // Only what this camera says it can do. A desktop webcam has no manual exposure mode, so
    // asking for one would fail arming outright — the honest answer there is a burst with the
    // locks it can have, and a line saying which it could not.
    const wanted = {
      exposure: lensLocks.supportsExposureLock,
      whiteBalance: lensLocks.supportsWhiteBalanceLock,
      focus: lensLocks.supportsFocusLock,
    };

    // Every failure ends up as `false` plus a line on screen, thrown ones included, and the try
    // starts here rather than at the arming call because both awaits are inside it. The callers
    // are a click handler and the end-to-end hook, neither of which awaits — so an exception
    // escaping is an unhandled rejection rather than anything a user could see, and a dead
    // worker or a track that vanished mid-gesture is exactly when that happens.
    // Everything the locks were applied for is off, so the camera goes back to metering.
    //
    // The core cannot do this one. `Disarm` is what releases the locks, and it returns early when
    // no burst is in flight — which is precisely the state a refused arm leaves behind. Until the
    // aim rule there was no refusal a user could actually reach (the burst spec is a constant, the
    // cell always exists, and the button is disabled while one is firing), so the ordering was
    // harmless; now a user pointing slightly off a cell can hit it, and what they would be left
    // with is a viewfinder frozen at one exposure and focus that pointing somewhere else does not
    // fix.
    const unlock = () => {
      remote.setLocks({ exposure: false, whiteBalance: false, focus: false });
      const had = lastLocksLine;
      void writeLocks({ exposure: false, whiteBalance: false, focus: false })
        .then((write) => showLocksReleased(locksOut, write, had));
      // And the row stops claiming them. It reads off the last successful *request*, so after a
      // refused arm it went on listing "exposure · white balance · focus" over a track that was
      // back to metering — the one row whose whole job is to be trustworthy about that. What it
      // must not do is forget *which* locks: "released" alone answers nothing, and a first attempt
      // that wrote it wholesale took out the row a browser test reads the camera's offered modes
      // from.
    };

    let held: Awaited<ReturnType<typeof camera.setLocks>>;
    let armedNow;
    try {
      const write = await writeLocks(wanted);
      if (!write.answered) {
        // No burst over a camera in an unknown state.
        //
        // The chain deliberately does not cancel the write — a cancelled one could be overtaken by
        // the release behind it and end a session locked — so a write this caller has given up on
        // is still going to reach the track, and on a merely slow camera it reaches it *during*
        // the burst that would be armed here. The five frames would then straddle an exposure and
        // focus change, which is the physical failure ADR 0022 exists to prevent, and nothing
        // downstream could explain it: the core would have been told no locks were held and the
        // row would have called it a refusal.
        //
        // Refusing costs one burst on a camera that answers late, and the retry works as soon as
        // the chain drains. Arming costs the burst anyway — quietly, and with pixels banked.
        //
        // `unlock` is what makes the camera agree with what it tells the core: the
        // release it queues sits behind the abandoned write, so whatever that write does to the
        // track is undone the moment the track is reachable again.
        // Recorded *before* the release is queued, not after. `unlock` snapshots `lastLocksLine`
        // to describe what its release is a release of, so setting the line one statement later
        // handed it the previous burst's record — or, on the first arm of a session, the empty
        // string, so the row flipped from "the camera did not answer the lock request" to "no
        // burst has run yet" when the release finally landed. Measured at t=7.5 s on a camera
        // taking 1500 ms per constraint. `da58ca7`'s snapshot is right for the `End()` collision
        // it was written for and wrong for a caller that queues its release above its own record.
        lastLocksLine = 'the camera did not answer the lock request';
        locksOut.textContent = lastLocksLine;
        unlock();
        sayForAWhile('the camera is not answering — not capturing');
        return false;
      }
      if (cameraLost()) {
        // Asked again, because the guard at the top of this function is a fact about the moment
        // the arm started and there is an await between them. A camera taken away mid-arm leaves
        // `camera.setLocks` still answering — the adapter's `active` stream is cleared only by its
        // own `close()`, which the page never calls, and `applyConstraints` on an ended track is
        // swallowed — so the write comes back `answered` and nothing below would notice. What that
        // arms is a burst the manager holds as firing for the life of the tab, over a loop that
        // has already stopped, with `#locks` rewritten to describe a camera that is gone.
        //
        // The same latch shape as `opened.ok` in `enable`, one function up, and it wants the same
        // answer: ask now rather than trusting a check that has aged across an await.
        // Recorded first, exactly as the branch fifteen lines above now does. This one was written
        // in the same commit as that fix and repeated the defect it was fixing: `unlock` snapshots
        // the row to say what its release is a release *of*, so queueing the release before writing
        // the record hands it the previous burst's line — or, on the first arm of a session, the
        // empty string, and the row ends terminally at "no burst has run yet" after the camera
        // really did take and release three locks.
        lastLocksLine = 'the camera was taken away mid-arm';
        locksOut.textContent = lastLocksLine;
        unlock();
        sayForAWhile('the camera was taken away — not capturing');
        return false;
      }
      held = write.done;
      // A camera that cannot lock still captures. What must not happen is the *core* believing a
      // lock is held when it is not, and pushing the confirmed state is what prevents that.
      const settled = held.ok
        ? held.value : { exposure: false, whiteBalance: false, focus: false };
      remote.setLocks(settled);
      // On screen as well as into the core. The page has always known which locks the camera
      // granted and had nowhere to say it, which left the one question a burst's numbers raise —
      // is the camera free to re-expose and refocus between these frames? — unanswerable from a
      // screenshot.
      // With what the track said it offers, read off the adapter rather than remembered here: it
      // belongs to the open camera, and a row explaining this refusal with the last camera's
      // lists would be worse than one that explained nothing (ADR 0033).
      lastLocksLine = describeLocks(wanted, held, camera.offeredModes());
      locksOut.textContent = lastLocksLine;

      armedNow = await core.captureSession.armBurst(node, {
        frameCount: 5, intervalMs: 80,
        // The locks above have just been applied, and a camera that takes a focus lock re-focuses
        // to honour it. The first frame waits for that rather than borrowing whatever the
        // viewfinder was showing mid-convergence — the contract's own default, restated here
        // because the wire carries every field and the page has to name one (ADR 0032).
        settleMs: 150,
        // Exactly what came back held, so the manager's own SetLocks matches the state the page
        // confirmed. Asking for a lock this camera did not take would fail arming; claiming one
        // it did not take would be worse — the burst would compare candidates on sharpness while
        // the exposure moved under it (ADR 0022).
        lockExposure: held.ok && held.value.exposure,
        lockWhiteBalance: held.ok && held.value.whiteBalance,
        lockFocus: held.ok && held.value.focus,
      });
    } catch (cause) {
      sayForAWhile(`arming failed: ${cause instanceof Error ? cause.message : String(cause)}`);
      unlock();
      return false;
    }
    if (cameraLost()) {
      // The third window, and it is not the harmless one I claimed on the thread. There is no page
      // route to `Disarm` — it carries no `@facade` marker — so a burst armed here does stay armed
      // whatever this returns, and that much of the decline was right. What was wrong is the rest:
      // `sayForAWhile` writes `#guidance` directly and no tick follows a stopped loop, so
      // "capturing without exposure lock" or an arming refusal becomes the page's last word for
      // the life of the tab, over a stage line telling the user to reload.
      sayForAWhile('the camera was taken away — not capturing');
      return false;
    }
    if (armedNow.ok) {
      armed = true;
      // Said out loud when the camera could not give a lock that was asked for. It is a real
      // quality cost — candidates that differ in exposure are compared on the wrong thing — and
      // it belongs on screen rather than in a comment nobody reads.
      const missing = (['exposure', 'whiteBalance', 'focus'] as const)
        .filter((lock) => wanted[lock] && !(held.ok && held.value[lock]));
      if (missing.length > 0) {
        sayForAWhile(`capturing without ${missing.join(', ')} lock`);
      }
      return true;
    }
    unlock();
    // The detail as well as the code. `FailedPrecondition` alone reads the same for "a burst is
    // already in flight" and "the camera is not aimed at that cell", and only one of those is
    // something the person holding the phone can do anything about.
    sayForAWhile(`arming failed: ${armedNow.status.code} — ${armedNow.status.detail}`);
    return false;
  };
  // Only with a plan: this loop also runs on the paths where capture could not start, so the
  // reason stays on screen and the sensor readout stays live. Arming there would fail with
  // NotFound against a plan that does not exist, which reads as a bug rather than as "no session".
  if (plan !== null) captureCell = armAt;

  // Whether this loop's camera was taken away under it.
  //
  // The narrower fact, not `!cameraHeld()` — see `cameraTakenAway`. An orderly `End` also leaves
  // no camera, and that must reach the ordinary failing-tick branch below rather than this one;
  // the first version of this check used the broad fact and made a session end announce "the
  // camera was taken away" while swallowing the guidance failure.
  //
  // The `plan !== null` half is what makes it about a session at all: this loop also runs with no
  // camera from the start, for a phone that has motion and nothing else, and there having no
  // camera is the ordinary state rather than a loss.
  const cameraLost = () => plan !== null && cameraTakenAway;

  const step = async () => {
    if (cameraLost()) {
      // The camera going away used to leave this loop running, and a burst armed after it banked
      // five candidates: the `<video>` keeps `readyState 4` and its dimensions after its track
      // ends, so both of the grabber's guards pass and every frame is a copy of the last one the
      // camera produced. Five sharp, well-scored frames of the same instant, filed under a cell —
      // the same undetectable-afterwards failure as ADR 0041's wrong pixels, from the other end.
      //
      // So the loop stops rather than degrades. A burst in flight stops with it, because
      // `AdvanceBurst` runs on this tick and on nothing else (ADR 0018) — no further tick means no
      // further candidate. The reticle and the markers go, because they describe where to point a
      // camera that is not there.
      // Same flag the guidance-failure branch sets, and for the same reason: `refreshCoverage` is
      // asynchronous, so one already in flight would land after the clear below and repaint the
      // rings and the arrow under a line saying the camera is gone. Not the map: `refreshCoverage`
      // calls `review.show` outside the flag's reach, deliberately — the sibling branch calls it
      // *for* the map — and a cell dot filling in late is late rather than false, unlike a marker,
      // which claims a direction relative to a pose that no longer exists. That branch got the
      // flag *and* a call-ordering fix after it was measured putting a ring back one millisecond
      // later; this early return copied the clear and neither protection.
      guidanceFailed = true;
      captureButton.disabled = true;
      overlay.show({ rings: [], arrow: null });
      guidanceOut.textContent = 'the camera was taken away';
      stage.textContent = 'the camera was taken away — reload to start again';
      return;
    }
    const drained = await motion.drain(32);
    const samples = drained.ok ? drained.value : [];

    // Handed to the host rather than through the facade: the core drains this buffer itself via
    // IMotionSensorAccess (ADR 0014), so the samples cross once as flat doubles instead of being
    // encoded a second time by the wire codec.
    if (samples.length > 0) remote.pushMotion(flattenImuSamples(samples.map(toImuSample)));

    if (samples.length > 0) {
      // The attitude, not whatever triple the platform happened to report: the two sources behind
      // the port speak different languages, and azimuth/elevation/roll is the one the plan is
      // written in (ADR 0017).
      attitude = samples[samples.length - 1].orientation;
      orientationOut.textContent = describeAttitude(attitude);
    }
    // Re-read rather than latched at start: the source can change mid-session, and with motion
    // off this line is carrying the reason why, which must not be overwritten with a description
    // of nothing.
    //
    // The failed drain carries its detail onto the line rather than being dropped. A quaternion
    // sensor that dies mid-session hands over asynchronously, with nobody left to return a
    // failure to, so a fallback that fails too used to leave the phone tracking nothing and the
    // readout saying only 'none'. Read every tick rather than latched, because the handover can
    // succeed on a later one.
    if (motionRunning) reportMotionSource(undefined, drained.ok ? '' : drained.status.detail);

    // Only when there is something new to fold in, plus once at the start so the reticle has a
    // position before the first sample arrives. An empty batch cannot change the pose, so it
    // cannot change the guidance — and a facade round trip per frame for an answer that cannot
    // have moved is a WASM call at 60Hz for nothing, which on a phone is heat and battery, and
    // under a loaded CI machine is enough to starve the rest of the suite.
    //
    // A burst in flight is the exception, and it is not an optimisation question. The burst
    // advances one frame per tick and on nothing else (ADR 0018), so skipping ticks while it is
    // firing does not merely freeze the reticle: the burst stalls, and it stalls holding the
    // camera's exposure lock. A sensor that has gone quiet — denied, absent, or just between
    // events — is exactly when that happens.
    // Before the tick that consumes it, which is the whole ordering requirement: the core reads
    // the frame synchronously from resident state, so it has to already be there (ADR 0021).
    // Only while a burst can use one — a grab is a draw plus a readback of megabytes, and doing
    // it every frame of every session would cost that for nothing.
    if (armed || firing) {
      const grabbed = grabFrame(viewfinder);
      if (grabbed !== null) remote.pushFrame(grabbed);
    }

    // A heartbeat, not a capability flag.
    //
    // Without something here the guard is false for ever after the first answer on a phone that
    // produces no samples: coverage still moves when a burst completes, and the target with it, so
    // such a capture froze on whatever guidance said at start-up, shutter included. The first
    // attempt asked `!motionRunning`, which is the wrong question twice over — it is latched from
    // `motion.start()`, so a sensor that starts and then stops delivering (a case the adapter
    // documents) still reads as running and freezes exactly as before; and on a device that really
    // has none it ticked every animation frame, sixty facade round trips a second for an answer
    // that mostly cannot have moved.
    //
    // Elapsed time answers both: whatever the reason the samples stopped, guidance is never more
    // than this stale, and the cost is four round trips a second rather than sixty.
    const quiet = performance.now() - lastGuidedMs > 250;
    if (plan !== null
        && (samples.length > 0 || !guidedOnce || firing || armed || quiet)) {
      lastGuidedMs = performance.now();
      guidedOnce = true;
      // Nothing passed: the manager drains the port, which is where the page just put them.
      const guided = await core.captureSession.onMotion([]);
      if (guided.ok) {
        guidanceFailed = false;
        const guidance = guided.value;
        firing = guidance.action === 'Firing';
        // Cleared once guidance has spoken for the armed burst, whatever it said. From here on
        // `firing` is the live answer and this flag would only keep the loop grabbing frames
        // after the burst had finished.
        if (guidance.action !== 'Seek') armed = false;
        targetNode = guidance.targetNode;
        // Offered only when a capture would actually be taken. The core refuses to arm a burst
        // against a cell the camera is not aimed at — a burst records what the camera sees, so
        // arming elsewhere files this direction's pixels under another cell's name — and a button
        // that reports "arming failed" for a rule the reticle already shows is a worse way to say
        // the same thing. `HoldStill` is precisely "inside the cone", which is precisely the
        // condition the core arms on.
        //
        // `AlreadyCaptured` is deliberately not offered *yet*: aiming at a captured cell is how a
        // re-capture will be asked for, and that wants the trigger PR #44 is about rather than a
        // button that silently re-shoots whatever the reticle rests on.
        //
        // This narrows the refusal; it does not make it unreachable, and the difference matters.
        // `aimKnown` is read from a guidance answer and `ArmBurst` re-reads the pose when the arm
        // arrives, so a press that crosses the tick where the first sample lands is offered under
        // one rule and judged under the next. That is a real `FailedPrecondition`, which is why
        // the refusal is still reported rather than treated as impossible.
        // No `captureCell === null` here: this whole block is inside `plan !== null`, and
        // `captureCell` is assigned exactly when a plan exists. The disjunct could never be true —
        // a guard implied by the one above it, which reads as defence and is not.
        //
        // And not only what guidance says. This line knew nothing about the arm the press had
        // just started, so the very next tick put the shutter back: measured at 41 ms after a
        // press, enabled again for 352 of the 393 ms before the core first reported `Firing`. The
        // second press inside that window was refused by `armAt`'s own guard, which is what made
        // it invisible — nothing was armed twice, and nothing said why the press did nothing.
        // A button offered while an arm is in flight is a button that lies about what pressing it
        // does.
        captureButton.disabled = !canCapture(guidance) || arming || armed || firing;
        const cone = cones.get(targetNode as number) ?? 0;
        // A closed reticle is a claim about where the camera is pointing, so it needs a pose that
        // says. With no aim, `angularErrorDeg` is measured from an unmeasured identity — it comes
        // back 0.00 for whichever cell happens to sit straight ahead, and the ring drew itself
        // fully closed and `locked` on a phone that has no idea where it is. Parked wide open
        // instead: honest, and it matches what the guidance line says (ADR 0042).
        const radius = guidance.aimKnown
          ? reticleRadius(guidance.angularErrorDeg, cone)
          : RETICLE_MAX_RADIUS;
        reticle.setAttribute('r', radius.toFixed(1));
        reticle.classList.toggle('locked', guidance.aimKnown && radius === RETICLE_LOCKED_RADIUS);
        // The horizon shows the roll the *core* reported against the target cell, not the raw
        // gamma from the sensor. Deriving it here would be the client deciding how level is
        // level enough, which is the planner's call (V4) and used to be wrong anyway: roll was
        // folded into the angular error until the engine started reporting it separately.
        // Same reasoning: a roll measured against an orientation nobody estimated is not a roll.
        horizonDeg = unwrapDegrees(horizonDeg, guidance.aimKnown ? -guidance.rollErrorDeg : 0);
        horizonGroup.setAttribute('transform', `rotate(${horizonDeg.toFixed(1)} 50 50)`);
        // Unless an arming message is still holding the line — see `sayForAWhile`. The reticle,
        // the horizon and the markers all keep updating; it is only this one sentence that waits.
        //
        // And only the *steady-state* sentence waits. `Seek`, `HoldStill` and `AlreadyCaptured`
        // are the aiming line, rewritten every frame, which is what the hold exists to survive.
        // The rest are events — a burst starting, a cell filling, a sphere finishing — and each
        // is true for one tick, so a hold that suppressed them would not delay the news, it would
        // delete it. It did: `capturing without … lock` swallowed the `CellDone` that followed it
        // and eight browser tests waiting for "captured" went red.
        // `Firing` belongs here despite being about an event: the manager reports it on *every*
        // tick of a burst, about thirty-three of them, so treating it as news let it bypass the
        // hold and wipe `capturing without … lock` after a measured 33 ms — which is the one
        // message the hold was introduced for. "Event" means true for one tick, and `Firing` is
        // not.
        const routine = guidance.action === 'Seek' || guidance.action === 'HoldStill'
          || guidance.action === 'AlreadyCaptured' || guidance.action === 'Firing';
        const line = describeGuidance(guidance, {
          nodesTotal, nodesSatisfied, coveredSolidAngleFraction: 0, holes: [],
          underOverlapped: [],
        });
        if (!routine) {
          // An event, and it is true for exactly one tick — `CellDone` most of all. It gets the
          // same hold the arming messages get, for the same reason: the aiming line is rewritten
          // whenever guidance answers, which the heartbeat now guarantees happens within 250 ms
          // even on a phone that has gone still. Before that, "captured" survived until the next
          // sample, which on a resting phone could be a long time — the message was readable by
          // accident, and making the loop reliable took the accident away.
          sayForAWhile(line);
        } else if (performance.now() >= guidanceHeldUntilMs) {
          guidanceOut.textContent = line;
        }
        // A cell finishing is the one thing that moves coverage, so it is the one thing that
        // redraws the map — and it is also where `nodesSatisfied` starts being a real number
        // rather than the zero the guidance line has been reporting since it was written.
        // Markers for what the plan says is out there, from the pose this tick produced. Drawn
        // from the coverage the map already keeps, so the rings and the map cannot disagree about
        // which cells are done — two answers to that question is how the strip ended up in an
        // order nothing had chosen.
        paintOverlay();
        if (guidance.action === 'CellDone') void refreshCoverage();
      } else {
        // Safe to stop ticking, because the manager disarms an armed burst on every failing tick
        // before it returns — so a failure means the burst really is gone and the camera's locks
        // are back. It was not always: clearing this while the manager left the burst armed is
        // what turned a stranded lock into a permanently stranded one.
        firing = false;
        armed = false;
        // What stops the repaint `refreshCoverage` would otherwise do at the end of this branch.
        guidanceFailed = true;
        // And nothing is offered. A failed tick produced no guidance, so there is no cell to
        // capture and no aim to have checked — leaving the shutter enabled meant a press that
        // cycled the camera's locks and changed nothing on screen, under a line saying guidance
        // had failed.
        captureButton.disabled = true;
        // And the markers go with it. They describe where the cells are *relative to a pose*, and
        // a failed tick is one that produced no pose — leaving the last set on screen would draw
        // a confident answer over a line that says guidance has stopped working.
        overlay.show({ rings: [], arrow: null });
        guidanceOut.textContent = `guidance failed: ${describeGuidanceFailure(guided.status)}`;
        // Coverage is re-read even though the tick failed, because a tick can fail *after* the
        // burst it was advancing has already committed: `AdvanceBurst` banks the cell and then
        // releases the locks, and a track that refuses to drop them turns the whole call into a
        // failure. The cell is captured, the core says so, and `CellDone` was never emitted — this
        // line is the only thing that redraws the map or moves the progress count, so without it
        // the page shows a stale count and a hole where a captured cell is, for the rest of the
        // session. Reading coverage is cheap and cannot make anything worse.
        //
        // Last in the branch, after the clear above and never before it: `refreshCoverage` repaints
        // the markers and is asynchronous, so a refresh started earlier landed *after*
        // `overlay.show({rings: []})` and put a ring back under a line saying guidance had stopped
        // working. Measured as `n:0` at t=683 and `n:1` at t=684.
        void refreshCoverage();
      }
    }

    requestAnimationFrame(step);
  };
  requestAnimationFrame(step);
}

/**
 * Exercises the boundary with a real call rather than reporting that it exists.
 *
 * ProjectManager.list is the honest choice: it needs no resource-access port, so what it proves
 * is the marshalling round trip — encode, dispatch, decode a Result — and nothing about a
 * capture pipeline that is not built.
 *
 * The summaries are handed back rather than counted and dropped: the same listing is what says
 * whether there is a capture to come back to, and asking twice for one answer would be a second
 * round trip for a question already answered (ADR 0036).
 */
async function reportFacade(core: SphanoramaCore): Promise<ProjectSummary[]> {
  const listed = await core.project.list();
  facadeOut.textContent = listed.ok
    ? `${core.methods().length} methods · ${listed.value.length} projects`
    : `call failed: ${listed.status.code}`;
  return listed.ok ? listed.value : [];
}

async function main() {
  try {
    const connected = await startCore();
    remote = connected.remote;
    const core = connected.core;

    // The core asked for the camera to be closed. Only this side is holding the tracks.
    remote.onCloseCamera(() => {
      stopCameraStream(cameraStream);
      cameraStream = null;
      // The row described a track that no longer exists, and it is module scope so nothing else
      // would have cleared it — a second session's first refusal would have quoted the first
      // session's locks.
      lastLocksLine = '';
    });

    // The core is done with the burst and wants the camera metering again. Only this side holds
    // the track, and nothing waits for it: the burst is already over (ADR 0022).
    remote.onReleaseLocks(() => {
      // The row says what the *last burst* got, and the burst is over — so it went on listing locks
      // the track had just given back. It was made truthful after a refused arm and left stale
      // after every successful one, which is the more common path by far.
      const had = lastLocksLine;
      void writeLocks({ exposure: false, whiteBalance: false, focus: false })
        .then((write) => showLocksReleased(locksOut, write, had));
    });

    // Durability on the way out. A phone backgrounds a tab without warning, and pagehide is the
    // last event that reliably fires; visibilitychange covers the cases where it does not.
    const flush = () => { void remote.flush(); };
    window.addEventListener('pagehide', flush);
    document.addEventListener('visibilitychange', () => {
      if (document.visibilityState === 'hidden') flush();
    });

    renderCapabilities(await core.capabilities({
      hardwareConcurrency: navigator.hardwareConcurrency ?? 1,
      crossOriginIsolated: self.crossOriginIsolated,
    }), remote.canSpill());
    // Exposed for the end-to-end suite to drive the boundary directly. The client itself never
    // reads these; it holds `core` and `remote` in scope.
    Object.assign(window as unknown as Record<string, unknown>, {
      sphanoramaCore: core,
      sphanoramaHost: { flush: () => remote.flush() },
      // The end-to-end suite drives capture through the same path the button does, rather than
      // reaching into the core: arming outside the loop is the mistake this hook exists to avoid.
      sphanoramaCapture: () =>
        (targetNode === null ? undefined : captureCell?.(targetNode)) ?? Promise.resolve(false),
    });
    // Asked before anything is offered, and answered without touching the camera: whether a
    // project has a session to return to is a fact about the project, and finding it out by
    // *trying* would start the very capture the user has not chosen yet (ADR 0036).
    const resume = resumableProject(await reportFacade(core));
    resumeButton.hidden = resume === null;
    stage.textContent = resume === null
      ? 'core ready — enable the camera to continue'
      : 'core ready — resume the last capture, or enable the camera to start a new one';
    enableButton.addEventListener('click', () => { void enable(core, null); });
    resumeButton.addEventListener('click', () => {
      if (resume === null) return;
      // Disabled while the attempt runs rather than hidden by it, the same way the fresh-start
      // button is: `pickUp` decides whether this offer survives its own refusal, and hiding on
      // the way in would take that decision away from it.
      resumeButton.disabled = true;
      // A refused resume can put this button back (ADR 0039), and by then `enable` has usually
      // run: the camera is open, the motion permission has been answered, and the gesture that
      // did both is long spent. So a second press retries the session and nothing else — asking
      // for a camera already in hand is at best a wasted round trip and at worst a second
      // permission story.
      //
      // Read off the stream the page is holding rather than a flag beside it, because the two
      // would drift and the drift is reachable: `Resume` opens the camera and can still fail
      // after it, in `StartTracking`, and that failure closes the camera on the way out. The page
      // learns through `onCloseCamera` and stops the tracks. A flag saying "enabled" would still
      // say so, and the retry would begin a session against a camera nobody is holding.
      const attempt = cameraHeld()
        ? beginSession(core, motionIsRunning, resume)
        : enable(core, resume);
      void attempt.finally(() => { resumeButton.disabled = false; });
    });
    // The way out of a refused resume. Disabled while the attempt runs rather than hidden by it:
    // `beginSession` can return having started nothing — a project that could not be created is
    // the one path that does — and hiding on the way in would take away the only thing left to
    // press. What hides it is `pump`, once something is actually running.
    newCaptureButton.addEventListener('click', () => {
      newCaptureButton.disabled = true;
      void beginSession(core, motionIsRunning, null)
        .finally(() => { newCaptureButton.disabled = false; });
    });
    captureButton.addEventListener('click', () => {
      // Disabled on the press rather than on the tick that notices the burst.
      //
      // `armAt` refuses a second arm while one is in flight, which is what stopped a double tap
      // from stripping a live burst's locks — but it refuses *silently*, and the button stayed
      // live for a measured 120 ms while it did. A press that visibly does nothing is the same
      // report as a press that breaks something. The pump owns the state from the next tick on.
      if (targetNode === null) return;
      captureButton.disabled = true;
      void captureCell?.(targetNode);
    });
  } catch (cause) {
    // Three things can fail now rather than one — the worker starts, the module loads inside it,
    // the document store opens — so the detail is what says which, and it is worth showing.
    stage.textContent = 'The core failed to load.';
    coreCaps.textContent = String(cause);
    enableButton.disabled = true;
  }

  if ('serviceWorker' in navigator && import.meta.env.PROD) {
    navigator.serviceWorker.register(`${import.meta.env.BASE_URL}sw.js`).catch(() => {
      // Offline support is a bonus; a registration failure must not stop the app working.
    });
  }
}

void main();

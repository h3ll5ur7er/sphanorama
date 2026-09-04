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
  CapturePlan, CoverageState, NodeId, ProjectId, Quat,
} from '../../contracts/ts/contracts';
import { createCameraAccess } from './access/camera';
import { createMotionSensorAccess } from './access/motion';
import { flattenImuSamples, stopCameraStream } from './access/capture-host';
import { canvasDrawTarget, createFrameGrabber, GRAB_MAX_EDGE } from './access/preview-frame';
import { toImuSample } from './access/orientation';
import { describeFailure, formatCapabilities } from './clients/capture/status';
import {
  describeGuidance, reticleRadius, unwrapDegrees, RETICLE_LOCKED_RADIUS,
} from './clients/capture/guidance';
import { describeAttitude } from './clients/capture/attitude';
import { planOverlay } from './clients/capture/overlay';
import { createOverlayPainter } from './clients/capture/painter';
import { createReviewPanel, type ReviewPanel } from './clients/review/panel';

const el = <T extends Element>(id: string) => document.getElementById(id) as unknown as T;

const viewfinder = el<HTMLVideoElement>('viewfinder');
const horizonGroup = el<SVGGElement>('horizon-group');
const reticle = el<SVGCircleElement>('reticle');
const cellLayer = el<HTMLElement>('cell-layer');
const targetArrow = el<HTMLElement>('target-arrow');

// The marker layer ends where the panel begins, and the panel's height is not a constant: it grows
// when the review strip opens and shrinks when it closes. Measured rather than assumed, because a
// ring drawn over the status lines is both unreadable and a lie about where a cell is.
const panel = el<HTMLElement>('panel');
if (typeof ResizeObserver === 'function') {
  new ResizeObserver(() => {
    document.documentElement.style.setProperty('--panel-height', `${panel.offsetHeight}px`);
  }).observe(panel);
}
const stage = el('stage');
const coreCaps = el('core-caps');
const cameraState = el('camera-state');
const motionState = el('motion-state');
const orientationOut = el('orientation');
const guidanceOut = el('guidance');
const facadeOut = el('facade');
// Written once, at load, and never touched again: it describes the bundle rather than the session,
// and a line that can change is a line a screenshot cannot be trusted on. `define` replaces the
// identifier at build time (see vite.config.mjs); the fallback is for `vite dev`, which does not.
el('build').textContent =
  typeof __SPHANORAMA_BUILD__ === 'string' ? __SPHANORAMA_BUILD__ : 'unknown';
const enableButton = el<HTMLButtonElement>('enable');
const captureButton = el<HTMLButtonElement>('capture');
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
function reportMotionSource(capability?: string) {
  if (capability !== undefined) motionCapabilityShown = capability;
  motionState.textContent = `${motionCapabilityShown} · ${motion.source()}`;
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
 * Enabling has to happen inside a user gesture: iOS rejects the motion permission request
 * otherwise, and does so in a way indistinguishable from a decline.
 */
async function enable(core: SphanoramaCore) {
  enableButton.disabled = true;

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
    cameraStream = camera.stream();
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
  const started = await motion.start(60);
  if (started.ok) {
    reportMotionSource(capability.ok ? capability.value : 'unknown');
  } else {
    // The core is told None, and that is a supported configuration rather than a failure: the
    // manager puts PoseEngine into vision-only mode and no other component learns the difference
    // (docs/03 UC-4). Declining motion on iOS is the common way to land here.
    remote.setMotion('None');
    motionState.textContent = 'unavailable';
    // Only overwrite the stage line if the camera did not already claim it: two failures at once
    // should not hide the first one.
    if (opened.ok) stage.textContent = describeFailure(started.status);
  }

  enableButton.hidden = true;
  // The camera is what a session needs; motion only makes aiming easier. Refusing to capture
  // without it would turn a supported degraded mode into a dead end.
  if (opened.ok) await beginSession(core, started.ok);
  else if (started.ok) pump(core, null, true, null);
}

/**
 * Opens a project and a session, then hands the plan to the render loop.
 *
 * A project comes first because a session belongs to one — the manager writes the session's
 * documents through the project store, and a capture with nowhere to be saved is a demo.
 */
async function beginSession(core: SphanoramaCore, motionRunning: boolean) {
  const created = await core.project.create(`sphere ${new Date().toISOString().slice(0, 16)}`);
  if (!created.ok) {
    stage.textContent = describeFailure(created.status);
    return;
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
    return;
  }

  const plan = await core.captureSession.getPlan();
  if (!plan.ok) {
    stage.textContent = describeFailure(plan.status);
    pump(core, null, motionRunning, null);
    return;
  }

  stage.textContent = motionRunning
    ? `capturing — ${plan.value.nodes.length} cells planned`
    : `capturing without motion — ${plan.value.nodes.length} cells planned, aim by hand`;
  pump(core, plan.value, motionRunning, created.value as ProjectId);
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
  const nodesTotal = plan?.nodes.length ?? 0;

  // The review panel only exists once there is a plan to map and a project to record a choice
  // against. Both arrive together or not at all.
  const review: ReviewPanel | null = plan === null || project === null ? null : createReviewPanel(
    reviewElements,
    {
      candidates: (node) => core.captureSession.candidates(node),
      setSelection: (node, candidate) => core.project.setSelection(project, node, candidate),
    });

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
  };

  // Once at the start, and not only when a cell completes. Coverage can only *change* when one
  // does, which is why the refresh below is where it is — but a map that is drawn only on change
  // is empty at the moment it is most worth reading, which is before the capture, when it is what
  // tells you where to point.
  void refreshCoverage();
  let guidedOnce = false;
  // Whether the last answer said a burst was still filling. A burst advances on this tick and
  // nothing else (ADR 0018), so it has to keep running even when the sensor has gone quiet.
  let firing = false;
  // Set the moment a burst is armed and cleared when guidance stops saying Firing. It exists
  // because of the hole ADR 0018 named: the tick right after arming has no guidance yet, so
  // `firing` is still false on the one tick that most needs a frame waiting for it.
  let armed = false;
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
    let held: Awaited<ReturnType<typeof camera.setLocks>>;
    let armedNow;
    try {
      held = await camera.setLocks(wanted);
      // A camera that cannot lock still captures. What must not happen is the *core* believing a
      // lock is held when it is not, and pushing the confirmed state is what prevents that.
      remote.setLocks(
        held.ok ? held.value : { exposure: false, whiteBalance: false, focus: false });

      armedNow = await core.captureSession.armBurst(node, {
        frameCount: 5, intervalMs: 80,
        // Exactly what came back held, so the manager's own SetLocks matches the state the page
        // confirmed. Asking for a lock this camera did not take would fail arming; claiming one
        // it did not take would be worse — the burst would compare candidates on sharpness while
        // the exposure moved under it (ADR 0022).
        lockExposure: held.ok && held.value.exposure,
        lockWhiteBalance: held.ok && held.value.whiteBalance,
        lockFocus: held.ok && held.value.focus,
      });
    } catch (cause) {
      guidanceOut.textContent =
        `arming failed: ${cause instanceof Error ? cause.message : String(cause)}`;
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
        guidanceOut.textContent = `capturing without ${missing.join(', ')} lock`;
      }
      return true;
    }
    guidanceOut.textContent = `arming failed: ${armedNow.status.code}`;
    return false;
  };
  // Only with a plan: this loop also runs on the paths where capture could not start, so the
  // reason stays on screen and the sensor readout stays live. Arming there would fail with
  // NotFound against a plan that does not exist, which reads as a bug rather than as "no session".
  if (plan !== null) captureCell = armAt;

  const step = async () => {
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
    if (motionRunning) reportMotionSource();

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

    if (plan !== null && (samples.length > 0 || !guidedOnce || firing || armed)) {
      guidedOnce = true;
      // Nothing passed: the manager drains the port, which is where the page just put them.
      const guided = await core.captureSession.onMotion([]);
      if (guided.ok) {
        const guidance = guided.value;
        firing = guidance.action === 'Firing';
        // Cleared once guidance has spoken for the armed burst, whatever it said. From here on
        // `firing` is the live answer and this flag would only keep the loop grabbing frames
        // after the burst had finished.
        if (guidance.action !== 'Seek') armed = false;
        targetNode = guidance.targetNode;
        // Enabled from the first answer rather than when the loop starts: before guidance has
        // named a cell there is no target to capture at all.
        captureButton.disabled = captureCell === null;
        const cone = cones.get(targetNode as number) ?? 0;
        const radius = reticleRadius(guidance.angularErrorDeg, cone);
        reticle.setAttribute('r', radius.toFixed(1));
        reticle.classList.toggle('locked', radius === RETICLE_LOCKED_RADIUS);
        // The horizon shows the roll the *core* reported against the target cell, not the raw
        // gamma from the sensor. Deriving it here would be the client deciding how level is
        // level enough, which is the planner's call (V4) and used to be wrong anyway: roll was
        // folded into the angular error until the engine started reporting it separately.
        horizonDeg = unwrapDegrees(horizonDeg, -guidance.rollErrorDeg);
        horizonGroup.setAttribute('transform', `rotate(${horizonDeg.toFixed(1)} 50 50)`);
        guidanceOut.textContent = describeGuidance(guidance, {
          nodesTotal, nodesSatisfied, coveredSolidAngleFraction: 0, holes: [], underOverlapped: [],
        });
        // A cell finishing is the one thing that moves coverage, so it is the one thing that
        // redraws the map — and it is also where `nodesSatisfied` starts being a real number
        // rather than the zero the guidance line has been reporting since it was written.
        // Markers for what the plan says is out there, from the pose this tick produced. Drawn
        // from the coverage the map already keeps, so the rings and the map cannot disagree about
        // which cells are done — two answers to that question is how the strip ended up in an
        // order nothing had chosen.
        if (attitude !== null && lastCoverage !== null) {
          overlay.show(planOverlay({ plan, coverage: lastCoverage, attitude, targetNode }));
        }
        if (guidance.action === 'CellDone') void refreshCoverage();
      } else {
        // Safe to stop ticking, because the manager disarms an armed burst on every failing tick
        // before it returns — so a failure means the burst really is gone and the camera's locks
        // are back. It was not always: clearing this while the manager left the burst armed is
        // what turned a stranded lock into a permanently stranded one.
        firing = false;
        armed = false;
        guidanceOut.textContent = `guidance failed: ${guided.status.code}`;
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
 */
async function reportFacade(core: SphanoramaCore) {
  const listed = await core.project.list();
  facadeOut.textContent = listed.ok
    ? `${core.methods().length} methods · ${listed.value.length} projects`
    : `call failed: ${listed.status.code}`;
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
    });

    // The core is done with the burst and wants the camera metering again. Only this side holds
    // the track, and nothing waits for it: the burst is already over (ADR 0022).
    remote.onReleaseLocks(() => {
      void camera.setLocks({ exposure: false, whiteBalance: false, focus: false });
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
    await reportFacade(core);
    stage.textContent = 'core ready — enable the camera to continue';
    enableButton.addEventListener('click', () => { void enable(core); });
    captureButton.addEventListener('click', () => {
      if (targetNode !== null) void captureCell?.(targetNode);
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

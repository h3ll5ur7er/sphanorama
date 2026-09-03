/**
 * Capture client entry point.
 *
 * Wires the browser adapters to the WASM core and renders the result. It contains no business
 * logic on purpose: reticle placement, acceptance and coverage are manager and engine decisions
 * behind contracts, and a client that computed them here would have to be unwound later.
 */
import type { RuntimeCapabilities, SphanoramaCore } from './bridge/core';
import { connectCore, type RemoteCore } from './bridge/remote-core';
import type { CapturePlan, ProjectId } from '../../contracts/ts/contracts';
import { createCameraAccess } from './access/camera';
import { createMotionSensorAccess } from './access/motion';
import { flattenImuSamples, stopCameraStream } from './access/capture-host';
import { toImuSample } from './access/orientation';
import { describeFailure, formatCapabilities } from './clients/capture/status';
import {
  describeGuidance, reticleRadius, unwrapDegrees, RETICLE_LOCKED_RADIUS,
} from './clients/capture/guidance';
import { describeAttitude } from './clients/capture/attitude';

const el = <T extends Element>(id: string) => document.getElementById(id) as unknown as T;

const viewfinder = el<HTMLVideoElement>('viewfinder');
const horizonGroup = el<SVGGElement>('horizon-group');
const reticle = el<SVGCircleElement>('reticle');
const stage = el('stage');
const coreCaps = el('core-caps');
const cameraState = el('camera-state');
const motionState = el('motion-state');
const orientationOut = el('orientation');
const guidanceOut = el('guidance');
const facadeOut = el('facade');
const enableButton = el<HTMLButtonElement>('enable');

const camera = createCameraAccess(navigator.mediaDevices);
const motion = createMotionSensorAccess(window);

// The stream the page opened. It stays on this side because a MediaStream cannot cross to the
// worker the core runs in, so the host asks and the page stops (ADR 0019).
let cameraStream: MediaStream | null = null;

/** The page's end of the worker: what it pushes across, and the one thing the worker asks back. */
let remote: RemoteCore;

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

  const opened = await camera.open({ preferRearCamera: true });
  if (opened.ok) {
    // Pushed before the core is asked to begin: the plan is sized from the lens, and the core
    // reads the lens through a synchronous port that cannot wait for getUserMedia — nor for a
    // message still in flight.
    remote.setCamera(opened.value);
    // Held here so the core can ask for it to be stopped: Close is a synchronous port call and
    // cannot reach a MediaStream itself.
    cameraStream = camera.stream();
    cameraState.textContent = `${opened.value.maxWidth}×${opened.value.maxHeight}`;
    viewfinder.srcObject = camera.stream();
  } else {
    remote.setCamera(null);
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
  else if (started.ok) pump(core, null, true);
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
    pump(core, null, motionRunning);
    return;
  }

  const plan = await core.captureSession.getPlan();
  if (!plan.ok) {
    stage.textContent = describeFailure(plan.status);
    pump(core, null, motionRunning);
    return;
  }

  stage.textContent = motionRunning
    ? `capturing — ${plan.value.nodes.length} cells planned`
    : `capturing without motion — ${plan.value.nodes.length} cells planned, aim by hand`;
  pump(core, plan.value, motionRunning);
}

/**
 * The capture loop: drain the sensor, hand the samples to the manager, render what it says back.
 *
 * No pose estimation and no reticle placement happen here — both come back from the core. With
 * no session the loop still runs, so the sensor readout stays live and the reason capture did
 * not start remains on screen.
 */
function pump(core: SphanoramaCore, plan: CapturePlan | null, motionRunning: boolean) {
  const cones = new Map((plan?.nodes ?? []).map((node) => [node.id as number, node.acceptanceConeDeg]));
  // Read once: coverage only moves when a cell is captured, and a facade round trip per frame
  // for a number that cannot have changed is the kind of waste that shows up as a hot phone.
  let nodesSatisfied = 0;
  const nodesTotal = plan?.nodes.length ?? 0;
  let guidedOnce = false;
  // Whether the last answer said a burst was still filling. A burst advances on this tick and
  // nothing else (ADR 0018), so it has to keep running even when the sensor has gone quiet.
  let firing = false;
  // Accumulated rather than taken fresh each frame, so rolling past the ±180 seam turns the
  // horizon by the two degrees the hand moved and not by the 358 the number jumped.
  let horizonDeg = 0;

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
      orientationOut.textContent = describeAttitude(samples[samples.length - 1].orientation);
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
    if (plan !== null && (samples.length > 0 || !guidedOnce || firing)) {
      guidedOnce = true;
      // Nothing passed: the manager drains the port, which is where the page just put them.
      const guided = await core.captureSession.onMotion([]);
      if (guided.ok) {
        const guidance = guided.value;
        firing = guidance.action === 'Firing';
        const cone = cones.get(guidance.targetNode as number) ?? 0;
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
      } else {
        // Safe to stop ticking, because the manager disarms an armed burst on every failing tick
        // before it returns — so a failure means the burst really is gone and the camera's locks
        // are back. It was not always: clearing this while the manager left the burst armed is
        // what turned a stranded lock into a permanently stranded one.
        firing = false;
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
    Object.assign(window as unknown as Record<string, unknown>,
                  { sphanoramaCore: core, sphanoramaHost: { flush: () => remote.flush() } });
    await reportFacade(core);
    stage.textContent = 'core ready — enable the camera to continue';
    enableButton.addEventListener('click', () => { void enable(core); });
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

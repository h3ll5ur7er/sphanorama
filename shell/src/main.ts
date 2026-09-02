/**
 * Capture client entry point.
 *
 * Wires the browser adapters to the WASM core and renders the result. It contains no business
 * logic on purpose: reticle placement, acceptance and coverage are manager and engine decisions
 * behind contracts, and a client that computed them here would have to be unwound later.
 */
import { loadCore, type RuntimeCapabilities, type SphanoramaCore } from './bridge/core';
import type { CapturePlan, ProjectId } from '../../contracts/ts/contracts';
import { createCameraAccess } from './access/camera';
import { createMotionSensorAccess } from './access/motion';
import { createCaptureHost, type CaptureHost } from './access/capture-host';
import { createDocumentHost, type DocumentHost } from './access/document-host';
import { createIndexedDbStore } from './access/indexeddb-store';
import { toImuSample } from './access/orientation';
import { describeFailure, formatCapabilities } from './clients/capture/status';
import { describeGuidance, reticleRadius, RETICLE_LOCKED_RADIUS } from './clients/capture/guidance';

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

/** The page half of the camera and motion ports, read synchronously from C++ (ADR 0014). */
const captureHost = createCaptureHost();

async function instantiateCore(host: DocumentHost & CaptureHost): Promise<SphanoramaCore> {
  // Loaded at runtime rather than bundled: the module is an artifact of the C++ build, and the
  // two builds (ADR 0011) are selected by which one the deploy copied in.
  const factory = (await import(/* @vite-ignore */ `${import.meta.env.BASE_URL}core/sphanorama-core.js`)).default;
  // The host is installed on the module before the core runs, so its documents are already
  // resident when the first synchronous read arrives from C++ (ADR 0014).
  return loadCore(async () => factory({ sphHost: host }));
}

function renderCapabilities(capabilities: RuntimeCapabilities) {
  coreCaps.textContent = formatCapabilities(capabilities);
}

/**
 * Enabling has to happen inside a user gesture: iOS rejects the motion permission request
 * otherwise, and does so in a way indistinguishable from a decline.
 */
async function enable(core: SphanoramaCore) {
  enableButton.disabled = true;

  const opened = await camera.open({ preferRearCamera: true });
  if (opened.ok) {
    // Told to the host before the core is asked to begin: the plan is sized from the lens, and
    // the core reads the lens through a synchronous port that cannot wait for getUserMedia.
    captureHost.setCamera(opened.value);
    cameraState.textContent = `${opened.value.maxWidth}×${opened.value.maxHeight}`;
    viewfinder.srcObject = camera.stream();
  } else {
    captureHost.clearCamera();
    cameraState.textContent = 'unavailable';
    stage.textContent = describeFailure(opened.status);
  }

  const capability = await motion.capabilities();
  if (capability.ok) captureHost.setMotion(capability.value);
  const started = await motion.start(60);
  if (started.ok) {
    motionState.textContent = capability.ok ? capability.value : 'unknown';
  } else {
    captureHost.setMotion('None');
    motionState.textContent = 'unavailable';
    // Only overwrite the stage line if the camera did not already claim it: two failures at once
    // should not hide the first one.
    if (opened.ok) stage.textContent = describeFailure(started.status);
  }

  enableButton.hidden = true;
  if (opened.ok && started.ok) await beginSession(core);
  else if (started.ok) pump(core, null);
}

/**
 * Opens a project and a session, then hands the plan to the render loop.
 *
 * A project comes first because a session belongs to one — the manager writes the session's
 * documents through the project store, and a capture with nowhere to be saved is a demo.
 */
async function beginSession(core: SphanoramaCore) {
  const created = await core.project.create(`sphere ${new Date().toISOString().slice(0, 16)}`);
  if (!created.ok) {
    stage.textContent = describeFailure(created.status);
    return;
  }

  const begun = await core.captureSession.begin(created.value as ProjectId, {
    strategy: 'Rings',
    // Zero means "probe the camera": the manager reads the real lens through the camera port,
    // and a number invented here would silently override it.
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
    pump(core, null);
    return;
  }

  const plan = await core.captureSession.getPlan();
  if (!plan.ok) {
    stage.textContent = describeFailure(plan.status);
    pump(core, null);
    return;
  }

  stage.textContent = `capturing — ${plan.value.nodes.length} cells planned for your lens`;
  pump(core, plan.value);
}

/**
 * The capture loop: drain the sensor, hand the samples to the manager, render what it says back.
 *
 * No pose estimation and no reticle placement happen here — both come back from the core. With
 * no session the loop still runs, so the sensor readout stays live and the reason capture did
 * not start remains on screen.
 */
function pump(core: SphanoramaCore, plan: CapturePlan | null) {
  const cones = new Map((plan?.nodes ?? []).map((node) => [node.id as number, node.acceptanceConeDeg]));
  // Read once: coverage only moves when a cell is captured, and a facade round trip per frame
  // for a number that cannot have changed is the kind of waste that shows up as a hot phone.
  let nodesSatisfied = 0;
  const nodesTotal = plan?.nodes.length ?? 0;

  const step = async () => {
    const drained = await motion.drain(32);
    const samples = drained.ok ? drained.value : [];

    if (samples.length > 0) {
      const latest = samples[samples.length - 1];
      const { alpha, beta, gamma } = latest.orientation;
      orientationOut.textContent =
        `α ${alpha.toFixed(0)}° β ${beta.toFixed(0)}° γ ${gamma.toFixed(0)}°`;
      horizonGroup.setAttribute('transform', `rotate(${-gamma.toFixed(1)} 50 50)`);
    }

    if (plan !== null) {
      // Called every frame including with an empty batch: the contract says so, and it is what
      // keeps the reticle answering while the phone is held still.
      const guided = await core.captureSession.onMotion(samples.map(toImuSample));
      if (guided.ok) {
        const guidance = guided.value;
        const cone = cones.get(guidance.targetNode as number) ?? 0;
        const radius = reticleRadius(guidance.angularErrorDeg, cone);
        reticle.setAttribute('r', radius.toFixed(1));
        reticle.classList.toggle('locked', radius === RETICLE_LOCKED_RADIUS);
        guidanceOut.textContent = describeGuidance(guidance, {
          nodesTotal, nodesSatisfied, coveredSolidAngleFraction: 0, holes: [], underOverlapped: [],
        });
      } else {
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
  const documents = await createDocumentHost(createIndexedDbStore());
  // One object, because the core reaches for one `Module.sphHost`. Both halves are plain
  // closures over their own state, so composing them here costs nothing and keeps each testable
  // on its own.
  const host = { ...documents, ...captureHost };

  // Durability on the way out. A phone backgrounds a tab without warning, and pagehide is the
  // last event that reliably fires; visibilitychange covers the cases where it does not.
  const flush = () => { void documents.flush(); };
  window.addEventListener('pagehide', flush);
  document.addEventListener('visibilitychange', () => {
    if (document.visibilityState === 'hidden') flush();
  });

  try {
    const core = await instantiateCore(host);
    renderCapabilities(core.capabilities({
      hardwareConcurrency: navigator.hardwareConcurrency ?? 1,
      crossOriginIsolated: self.crossOriginIsolated,
    }));
    // Exposed for the end-to-end suite to drive the boundary directly. The client itself
    // never reads this; it holds `core` in scope.
    Object.assign(window as unknown as Record<string, unknown>,
                  { sphanoramaCore: core, sphanoramaHost: host });
    await reportFacade(core);
    stage.textContent = 'core ready — enable the camera to continue';
    enableButton.addEventListener('click', () => { void enable(core); });
  } catch (cause) {
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

/**
 * Capture client entry point.
 *
 * Wires the browser adapters to the WASM core and renders the result. It contains no business
 * logic on purpose: reticle placement, acceptance and coverage are manager and engine decisions
 * behind contracts, and a client that computed them here would have to be unwound later.
 */
import { loadCore, type RuntimeCapabilities, type SphanoramaCore } from './bridge/core';
import { createCameraAccess } from './access/camera';
import { createMotionSensorAccess } from './access/motion';
import { describeFailure, formatCapabilities } from './clients/capture/status';

const el = <T extends Element>(id: string) => document.getElementById(id) as unknown as T;

const viewfinder = el<HTMLVideoElement>('viewfinder');
const horizonGroup = el<SVGGElement>('horizon-group');
const stage = el('stage');
const coreCaps = el('core-caps');
const cameraState = el('camera-state');
const motionState = el('motion-state');
const orientationOut = el('orientation');
const enableButton = el<HTMLButtonElement>('enable');

const camera = createCameraAccess(navigator.mediaDevices);
const motion = createMotionSensorAccess(window);

async function instantiateCore(): Promise<SphanoramaCore> {
  // Loaded at runtime rather than bundled: the module is an artifact of the C++ build, and the
  // two builds (ADR 0011) are selected by which one the deploy copied in.
  const factory = (await import(/* @vite-ignore */ `${import.meta.env.BASE_URL}core/sphanorama-core.js`)).default;
  return loadCore(async () => factory());
}

function renderCapabilities(capabilities: RuntimeCapabilities) {
  coreCaps.textContent = formatCapabilities(capabilities);
}

/**
 * Enabling has to happen inside a user gesture: iOS rejects the motion permission request
 * otherwise, and does so in a way indistinguishable from a decline.
 */
async function enable() {
  enableButton.disabled = true;

  const opened = await camera.open({ preferRearCamera: true });
  if (opened.ok) {
    cameraState.textContent = `${opened.value.maxWidth}×${opened.value.maxHeight}`;
    viewfinder.srcObject = camera.stream();
  } else {
    cameraState.textContent = 'unavailable';
    stage.textContent = describeFailure(opened.status);
  }

  const capability = await motion.capabilities();
  const started = await motion.start(60);
  if (started.ok) {
    motionState.textContent = capability.ok ? capability.value : 'unknown';
    pump();
  } else {
    motionState.textContent = 'unavailable';
    // Only overwrite the stage line if the camera did not already claim it: two failures at once
    // should not hide the first one.
    if (opened.ok) stage.textContent = describeFailure(started.status);
  }

  if (opened.ok && started.ok) stage.textContent = 'live — sensors routed through the core';
  enableButton.hidden = true;
}

/** Drains buffered orientation and renders it. No pose estimation here — that is PoseEngine. */
function pump() {
  const step = async () => {
    const drained = await motion.drain(32);
    if (drained.ok && drained.value.length > 0) {
      const latest = drained.value[drained.value.length - 1];
      const { alpha, beta, gamma } = latest.orientation;
      orientationOut.textContent =
        `α ${alpha.toFixed(0)}° β ${beta.toFixed(0)}° γ ${gamma.toFixed(0)}°`;
      horizonGroup.setAttribute('transform', `rotate(${-gamma.toFixed(1)} 50 50)`);
    }
    requestAnimationFrame(step);
  };
  requestAnimationFrame(step);
}

async function main() {
  try {
    const core = await instantiateCore();
    renderCapabilities(core.capabilities({
      hardwareConcurrency: navigator.hardwareConcurrency ?? 1,
      crossOriginIsolated: self.crossOriginIsolated,
    }));
    stage.textContent = 'core ready — enable the camera to continue';
    enableButton.addEventListener('click', () => { void enable(); });
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

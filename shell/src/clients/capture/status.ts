/**
 * Turning core and adapter outcomes into words.
 *
 * This is the capture client's only judgement. Everything else it does is render what a manager
 * told it — reticle placement, coverage, acceptance are all business decisions that live behind
 * contracts, not here.
 */
import type { RuntimeCapabilities } from '../../bridge/core';
import type { Status } from '../../access/result';

const MESSAGES: Partial<Record<Status['code'], string>> = {
  SensorPermissionDenied:
    'Camera or motion permission was declined. Allow access in your browser settings, then reload.',
  SensorUnavailable:
    'This device reports no motion sensors. Capture will fall back to visual tracking.',
  CameraUnavailable:
    'No usable camera. Another tab of this app, or another app, may be holding it — '
    + 'close them and reload. Otherwise this device has no camera facing that way.',
  StorageQuotaExceeded:
    'Out of storage for this site. Free some space or delete an old sphere.',
  FrameStoreExhausted:
    'Ran out of memory for frames. Try a smaller sphere, or close other tabs.',
};

/**
 * Unsupported means "this build does not do that yet" all over the core — the burst path, the
 * build pipeline, resume — and each of those failures arrives carrying the reason the component
 * gave. Only the camera adapter's version of it is about a secure origin, so only that one is
 * rewritten; mapping the code alone would replace every other explanation with an https message
 * that has nothing to do with what failed.
 */
const CAMERA_ADAPTER = 'CameraAccess';

export function describeFailure(status: Status): string {
  if (status.code === 'Unsupported' && status.component === CAMERA_ADAPTER) {
    return 'The camera needs a secure origin. This page must be served over https.';
  }
  // Falling back to the raw detail is deliberate: an unmapped code with no words is still more
  // useful than "something went wrong", and it makes the gap visible enough to fix.
  return MESSAGES[status.code] ?? (status.detail || `${status.component} failed (${status.code}).`);
}

export function formatCapabilities(capabilities: RuntimeCapabilities, canSpill: boolean): string {
  const parts: string[] = [];
  parts.push(capabilities.threads
    ? `${capabilities.hardwareConcurrency} threads`
    : 'single-threaded');
  parts.push(capabilities.simd ? 'SIMD' : 'no SIMD');
  if (!capabilities.sharedMemory) parts.push('no shared memory');
  // Only when it is missing. A capture with nowhere to spill is capped at what fits in RAM and
  // will refuse partway through a sphere (ADR 0020); saying so beforehand is the difference
  // between a known limit and an inexplicable failure. Naming the ordinary case would be noise.
  if (!canSpill) parts.push('no spill tier');
  return parts.join(' · ');
}

/** The three things a burst can hold still while it fires (ADR 0022). */
export interface LockState {
  exposure: boolean;
  whiteBalance: boolean;
  focus: boolean;
}

const LOCK_NAMES: [keyof LockState, string][] = [
  ['exposure', 'exposure'],
  ['whiteBalance', 'white balance'],
  ['focus', 'focus'],
];

/**
 * What the camera actually let this burst hold still, against what it was asked for.
 *
 * A burst of five frames at 80 ms spans about a third of a second, and with nothing locked that is
 * the camera's to spend re-exposing and refocusing. Frames from one cell can then differ by more
 * than the scene does — which showed up on a Pixel as a burst whose sharpness ran 1186, 1180, 459,
 * 458, 458: two frames from one regime and three from another, in a strip that gave no way to tell
 * whether that was the camera hunting or something the selection policy did.
 *
 * Held and refused are told apart on purpose. A refused lock is a camera that advertised a manual
 * mode and then did not take it, which is the case ADR 0022's read-back exists for; a lock that
 * was never asked for is a camera that said up front it has no manual mode. Reporting both as
 * absence would hide the first behind the second.
 */
export function describeLocks(wanted: LockState, held: LockState): string {
  const holding = LOCK_NAMES.filter(([key]) => held[key]).map(([, name]) => name);
  // Read off `held` rather than `wanted`: `advanced` constraints are best-effort in both
  // directions, and a camera is free to settle on manual for its own reasons. The burst is armed
  // with what actually holds, so this line has to be that and not a copy of the request.
  const refused = LOCK_NAMES.filter(([key]) => wanted[key] && !held[key])
    .map(([, name]) => `${name} refused`);

  const parts = [...holding, ...refused];
  if (parts.length === 0) return 'none — this camera offers no manual modes';
  return parts.join(' · ');
}

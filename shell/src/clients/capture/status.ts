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
    'No usable camera. It may be missing, or another app may be holding it.',
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

export function formatCapabilities(capabilities: RuntimeCapabilities): string {
  const parts: string[] = [];
  parts.push(capabilities.threads
    ? `${capabilities.hardwareConcurrency} threads`
    : 'single-threaded');
  parts.push(capabilities.simd ? 'SIMD' : 'no SIMD');
  if (!capabilities.sharedMemory) parts.push('no shared memory');
  return parts.join(' · ');
}

/**
 * Turning core and adapter outcomes into words.
 *
 * This is the capture client's only judgement. Everything else it does is render what a manager
 * told it — reticle placement, coverage, acceptance are all business decisions that live behind
 * contracts, not here.
 */
import type { RuntimeCapabilities } from '../../bridge/core';
import type { Result, Status } from '../../access/result';
import type { LockReport } from '../../bridge/protocol';

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

/**
 * A failed tick, for the line under the reticle.
 *
 * The code alone is what an iPhone reported when its frame store ran out — `FrameStoreExhausted`
 * and nothing else — and the store had in fact said which of two opposite problems it was: a
 * sphere too large for the device, or a spill sink that refused a write and left the heap holding
 * what it hoped to give back (ADR 0023). One is "capture less", the other is "free some disk",
 * and the sentence that told them apart was being dropped at the call site.
 *
 * The code stays because it is short, greppable, and the same word the logs use; the detail joins
 * it when the component had one to give.
 */
export function describeGuidanceFailure(status: Status): string {
  return status.detail ? `${status.code} — ${status.detail}` : status.code;
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

// The shape the page confirmed and pushed to the core, not a third copy of it. `ICameraAccess`
// takes the three locks as positional booleans, so the contract generates no struct to share and
// the camera adapter keeps its own `LockState` — a client must not depend on a resource access,
// which is the one edge naming the same fields twice buys.
const LOCK_NAMES: [keyof LockReport, string][] = [
  ['exposure', 'exposure'],
  ['whiteBalance', 'white balance'],
  ['focus', 'focus'],
];

/**
 * What the camera said it can do for each control, or null where the browser did not say.
 *
 * Named here rather than imported from the camera adapter for the same reason `LockReport` is:
 * a client must not depend on a resource access. Nothing pushes this to the core — the contract
 * has no field for it and no component would decide anything with one — so unlike the lock report
 * there is no crossing type to borrow, and the client states the shape it renders (ADR 0033).
 */
export interface LockModes {
  exposure: readonly string[] | null;
  whiteBalance: readonly string[] | null;
  focus: readonly string[] | null;
}

/**
 * What one control offers, as a phrase for a refusal to be read against.
 *
 * The modes are named rather than summarised. "Offers continuous" and "offers continuous, manual"
 * are the two halves of the question a refusal raises — is there a lock here at all, or is this
 * camera contradicting itself? — and any wording that decides which of those the reader is
 * looking at is a guess this row exists to stop making.
 */
function offering(modes: readonly string[] | null): string {
  if (modes === null) return 'not reported';
  if (modes.length === 0) return 'offers nothing';
  return `offers ${modes.join(', ')}`;
}

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
 *
 * `offered` is what the camera said it can do, and a refusal is written against it because
 * "refused" on its own does not say which of those two a reader is looking at — that ambiguity is
 * what left a Pixel guessed at twice (ADR 0033). It is evidence for the sentence and nothing more:
 * what to ask the camera for is decided without it, by asking.
 */
export function describeLocks(wanted: LockReport, held: Result<LockReport>,
                              offered: LockModes): string {
  // Asking is itself fallible — there is no camera to put a question to once a track has been
  // pulled away — and a failure rendered as three absent locks reads as "this camera has no
  // manual modes". That is the one sentence this row exists to make trustworthy, and it would
  // send the next reader after bracketing for a camera nothing ever asked. The burst is still
  // armed, with no locks claimed: whether a camera that cannot answer can capture at all is the
  // manager's to decide, and it abandons the burst on the first tick that yields no frame.
  if (!held.ok) return `unknown — ${held.status.detail || held.status.code}`;

  const holding = LOCK_NAMES.filter(([key]) => held.value[key]).map(([, name]) => name);
  // Read off `held` rather than `wanted`: `advanced` constraints are best-effort in both
  // directions, and a camera is free to settle on manual for its own reasons. The burst is armed
  // with what actually holds, so this line has to be that and not a copy of the request.
  //
  // And carrying what the camera says it has, because "refused" alone is the reading that sent us
  // guessing: a camera that lists a manual mode and will not take it and a camera that lists none
  // are opposite problems, and the row said the same word for both. Only refusals carry it — a
  // lock that is holding raises no question worth the width.
  const refused = LOCK_NAMES.filter(([key]) => wanted[key] && !held.value[key])
    .map(([key, name]) => `${name} refused (${offering(offered[key])})`);

  const parts = [...holding, ...refused];
  if (parts.length > 0) return parts.join(' · ');

  // Nothing held and nothing asked for, which is a camera with no manual modes — or a browser
  // that was never going to say. The second is not the first: these keys are optional and a
  // browser fills them in as it likes, so a row concluding "this camera offers no manual modes"
  // from that silence is the same invention one line up, made about the whole camera at once.
  const lists = LOCK_NAMES.map(([key]) => offered[key]);
  if (lists.every((modes) => modes === null)) {
    return 'none — this browser does not report what the camera offers';
  }
  if (lists.every((modes) => modes !== null)) return 'none — this camera offers no manual modes';
  // Answered for some controls and not others. Neither sentence above is true of this camera, so
  // the row says which is which rather than rounding to whichever is closer.
  return `none — ${LOCK_NAMES.map(([key, name]) => `${name} ${offering(offered[key])}`).join(' · ')}`;
}

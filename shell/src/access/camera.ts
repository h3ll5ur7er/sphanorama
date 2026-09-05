/**
 * Browser implementation of ICameraAccess — the parts of it that are asynchronous.
 *
 * Opening a camera and applying a lock both return promises and both can be refused, so neither
 * fits a synchronous port. They live here, on the page, and what the core reads is the state they
 * established: capabilities after `open`, and which locks are actually held after `setLocks`
 * (ADR 0014, ADR 0022). Pixels take the same route by a different road — grabbed from the
 * viewfinder and transferred (ADR 0021).
 *
 * The mapping from getUserMedia's error names onto StatusCodes is the other substance of this
 * file. A user who declined and a device with no camera need different words on screen, and the
 * difference has to survive the trip to the core.
 */
import { err, ok, type Result } from './result';

const COMPONENT = 'CameraAccess';

export interface CameraOpenSpec {
  preferRearCamera: boolean;
  preferredWidth?: number;
  preferredHeight?: number;
}

export interface CameraCapabilities {
  maxWidth: number;
  maxHeight: number;
  supportsTorch: boolean;
  /**
   * Whether the track offers a *manual* mode for each. Reported rather than assumed, because a
   * lock this camera cannot take is one a burst would otherwise believe it had: candidates would
   * be compared on sharpness while the camera kept metering between them (ADR 0022).
   */
  supportsExposureLock: boolean;
  supportsWhiteBalanceLock: boolean;
  supportsFocusLock: boolean;
}

/** Which locks are actually held right now — the answer, not the request. */
export interface LockState {
  exposure: boolean;
  whiteBalance: boolean;
  focus: boolean;
}

/**
 * What the track said it can do for each control, as strings.
 *
 * Not verbatim: `getCapabilities()` returns a dictionary the browser fills in as it likes, so the
 * entries are coerced with `String` rather than trusted to be strings already. A mode is only ever
 * shown to a reader, so a value that is not a string has nothing to lose by being rendered as one
 * — and a row that threw while formatting a status line would be worse than one naming something
 * odd.
 *
 * `null` is a different answer from an empty list, and keeping them apart is the whole reason
 * this exists. A camera that lists `["continuous"]` has told us it has no lock to give; a track
 * with no `getCapabilities` at all, one that throws, or one whose dictionary simply omits the key
 * has told us nothing. Rendering both as absence is how `exposure refused` came to mean two
 * opposite things and we guessed twice at which. Every one of these is real: the browser fills
 * that dictionary in as it likes, and Chromium's own fake device omits `whiteBalanceMode` while
 * listing the other two.
 *
 * Reported, never consulted. Nothing in this file reads these lists to decide what to ask the
 * camera for — see the note on `HOLDING_MODES` and ADR 0033.
 */
export interface LockModes {
  exposure: readonly string[] | null;
  whiteBalance: readonly string[] | null;
  focus: readonly string[] | null;
}

// Frozen because it is shared. Every camera that reports nothing hands back this same value, so
// one write to it is not one corrupted row — it is every row for the life of the tab.
const NOTHING_REPORTED: LockModes =
  Object.freeze({ exposure: null, whiteBalance: null, focus: null });

export interface CameraAccess {
  open(spec: CameraOpenSpec): Promise<Result<CameraCapabilities>>;
  stream(): MediaStream | null;
  /**
   * Asks the track for the modes these locks imply and reports back what it settled on.
   *
   * The return value is deliberately the *observed* state rather than an acknowledgement: a
   * browser may resolve `applyConstraints` and leave the mode where it was, and a caller told
   * "applied" would then fire a burst believing its exposure fixed. Reading the settings back is
   * the only thing that makes the claim true (ADR 0022).
   */
  setLocks(wanted: LockState): Promise<Result<LockState>>;
  /**
   * The mode lists the open track reported, for a refusal to be read against. Frozen: it is the
   * adapter's own state, handed out to be read.
   *
   *
   * Separate from `open`'s capabilities rather than a field on them, because those cross to the
   * core as `CameraCapabilities` and this does not: the contract has no place for a list of
   * browser mode strings, and nothing in the core would decide anything with one. It is a page
   * fact, for the page to say out loud (ADR 0033).
   */
  offeredModes(): LockModes;
  close(): Promise<Result<void>>;
}

/**
 * The modes that mean "stop adapting", in the order worth asking for them.
 *
 * `manual` is the strongest — it fixes the value — but on Android it generally means "and I will
 * tell you the number", so a camera asked to go manual with nothing else in the set refuses.
 * `single-shot` is the weaker promise that costs nothing to make: converge once, then hold.
 * Either keeps a burst's frames comparable, which is all this is for (ADR 0022).
 *
 * Both are asked for whatever the track's capabilities say, and deliberately so: browsers
 * under-report, a device may take a constraint it never advertised, and one that lists nothing
 * would get no locks at all from a negotiation that read the list first (ADR 0033).
 */
const HOLDING_MODES = ['manual', 'single-shot'] as const;
const ADAPTING_MODE = 'continuous';

/** A capability array offers a lock when it contains any mode that stops the camera adapting. */
function offersManual(modes: unknown): boolean {
  return Array.isArray(modes) && HOLDING_MODES.some((mode) => modes.includes(mode));
}

/**
 * A capability entry as something to report: the modes it lists, or null for no answer.
 *
 * A key present but not a list is no answer either — `getCapabilities` returns a dictionary the
 * browser fills in as it likes, and something that is not an enumeration says nothing about the
 * camera that an absent key does not.
 */
function reportedModes(modes: unknown): readonly string[] | null {
  return Array.isArray(modes) ? Object.freeze(modes.map(String)) : null;
}

const MODE_OF: Record<keyof LockState, string> = {
  exposure: 'exposureMode',
  whiteBalance: 'whiteBalanceMode',
  focus: 'focusMode',
};

/** Whether the settings this track reports say the camera has stopped moving this control. */
function holding(settings: Record<string, unknown>, key: keyof LockState): boolean {
  return HOLDING_MODES.includes(settings[MODE_OF[key]] as typeof HOLDING_MODES[number]);
}

/**
 * One constraint set: this lock, this mode, and whatever else that mode needs to be satisfiable.
 *
 * One set per lock rather than all three in one, which is the whole point. An advanced set is
 * applied only if the *whole* of it can be satisfied, so a mode the camera will not take discards
 * the ones it would have — a Pixel reported `focus · exposure refused · white balance refused`
 * and had in fact refused nothing but the exposure.
 */
function constraintFor(key: keyof LockState, mode: string,
                       settings: Record<string, unknown>): Record<string, unknown> {
  const set: Record<string, unknown> = { [MODE_OF[key]]: mode };
  // The number a manual exposure needs, taken from what the camera is metering at right now —
  // which is the exposure the cell was framed at, and the one the burst wants held.
  if (key === 'exposure' && mode === 'manual' && typeof settings.exposureTime === 'number') {
    set.exposureTime = settings.exposureTime;
  }
  return set;
}

/** getUserMedia's failure vocabulary, translated once. */
function statusFor(name: string) {
  switch (name) {
    case 'NotAllowedError':
    case 'SecurityError':
      return 'SensorPermissionDenied' as const;
    case 'NotFoundError':
    case 'OverconstrainedError':
    case 'NotReadableError':   // the camera exists but another app holds it
      return 'CameraUnavailable' as const;
    default:
      return 'Internal' as const;
  }
}

export function createCameraAccess(media: MediaDevices | undefined): CameraAccess {
  let active: MediaStream | null = null;
  // What this camera has already said no to, so a sphere does not ask twenty-eight times. Locks
  // are applied before every burst, and each attempt is a round trip sitting between framing a
  // cell and capturing it. Cleared wherever the track changes — both ends of it, `open` as well
  // as `close`, because a refusal belongs to the camera that made it.
  let hopeless = new Set<keyof LockState>();
  // What this track answered when it was asked what it can do. Cleared with the track for the
  // same reason the refusals are: it describes that camera, and a list left standing after a
  // close would explain the next refusal with the last camera's evidence.
  let offered: LockModes = NOTHING_REPORTED;

  return {
    async open(spec: CameraOpenSpec) {
      if (!media?.getUserMedia) {
        // No mediaDevices means an insecure origin far more often than a device without a camera.
        return err<CameraCapabilities>('Unsupported', COMPONENT,
          'no media devices — the page must be served over https');
      }

      // Everything the caller asked for that is genuinely negotiable. `ideal` is scored rather
      // than obeyed: getUserMedia picks whichever device has the lowest *combined* fitness
      // distance across all of these at once.
      const size = {
        width: spec.preferredWidth ? { ideal: spec.preferredWidth } : undefined,
        height: spec.preferredHeight ? { ideal: spec.preferredHeight } : undefined,
      };
      const facing = spec.preferRearCamera ? 'environment' : 'user';

      try {
        // Which way the camera faces is `exact`, which makes it a filter instead of a score, and
        // so takes it out of that competition entirely. Asked as an `ideal` alongside a
        // resolution, a front camera that matches the requested size more closely outscores a
        // rear one that does not — and the app shoots a photo sphere as a selfie. That is not
        // hypothetical: it is what a Pixel 9 Pro XL did the first time a resolution was asked
        // for, and nothing on screen said which lens had been chosen.
        let stream: MediaStream;
        try {
          stream = await media.getUserMedia({
            video: { ...size, facingMode: { exact: facing } },
            audio: false,
          });
        } catch (cause) {
          // A filter has no second choice, so a device with no camera facing that way answers
          // OverconstrainedError and would get no camera at all. That is the one case where the
          // direction really is a preference: a laptop should still see its only lens.
          if ((cause as { name?: string }).name !== 'OverconstrainedError') throw cause;
          stream = await media.getUserMedia({
            video: { ...size, facingMode: { ideal: facing } },
            audio: false,
          });
        }
        // The one it was already holding goes first. Nothing else is holding that stream, so a
        // second open would leave it running for the life of the page with the indicator lit —
        // which a user reads, correctly, as the app watching them. And what a camera will not do
        // is a fact about *that* camera: carrying a refusal across an open is how the next one
        // silently loses a lock it would have given.
        active?.getTracks().forEach((track) => track.stop());
        hopeless = new Set();
        offered = NOTHING_REPORTED;
        active = stream;

        // Report what the track settled on, not what we asked for: requested and granted
        // resolution differ constantly across devices, and the coverage plan is sized from the
        // real field of view.
        const track = stream.getVideoTracks()[0];
        const settings = track?.getSettings() ?? {};
        // Three ways for this to say nothing, and all three end in the same empty object: no
        // `getCapabilities` on the track, a call that throws, or a dictionary without the keys.
        // The throw is why this is a try rather than an `?.`: it used to escape into the catch
        // below and report a camera that had just opened as unavailable, over a question asked
        // for a status line.
        let capabilities: Record<string, unknown> = {};
        try {
          capabilities = (track?.getCapabilities?.() ?? {}) as Record<string, unknown>;
        } catch {
          // Nothing reported, which is exactly what an absent API means too.
        }
        // Frozen, not copied on the way out. `readonly` is a compile-time promise and the
        // accessor hands this to a caller as an ordinary object; the row is rendered on the
        // capture tick, so a defensive copy per call would be garbage per frame to defend
        // against something that can only happen per camera. This makes the promise real once.
        offered = Object.freeze({
          exposure: reportedModes(capabilities.exposureMode),
          whiteBalance: reportedModes(capabilities.whiteBalanceMode),
          focus: reportedModes(capabilities.focusMode),
        });
        return ok({
          maxWidth: settings.width ?? 0,
          maxHeight: settings.height ?? 0,
          supportsTorch: 'torch' in capabilities,
          supportsExposureLock: offersManual(capabilities.exposureMode),
          supportsWhiteBalanceLock: offersManual(capabilities.whiteBalanceMode),
          supportsFocusLock: offersManual(capabilities.focusMode),
        });
      } catch (cause) {
        const error = cause as { name?: string; message?: string };
        return err<CameraCapabilities>(statusFor(error.name ?? ''), COMPONENT,
          `${error.name ?? 'Error'}: ${error.message ?? String(cause)}`);
      }
    },

    stream() {
      return active;
    },

    offeredModes() {
      return offered;
    },

    async setLocks(wanted: LockState) {
      const track = active?.getVideoTracks()[0] as (MediaStreamTrack & {
        applyConstraints?(constraints: unknown): Promise<void>;
      }) | undefined;
      if (!track) {
        return err<LockState>('CameraUnavailable', COMPONENT, 'no camera open');
      }

      // A refusal is never a failure of this call: it means the camera would not take that lock,
      // which is exactly what the returned state is for. Reporting it as an error would make the
      // caller choose between no burst and a burst it knows nothing about.
      const ask = async (set: Record<string, unknown>) => {
        try {
          await track.applyConstraints?.({ advanced: [set] });
        } catch {
          // Silence and refusal look the same from here, and the read-back tells them apart.
        }
      };

      for (const key of Object.keys(MODE_OF) as (keyof LockState)[]) {
        if (!wanted[key]) {
          await ask({ [MODE_OF[key]]: ADAPTING_MODE });
          continue;
        }
        if (hopeless.has(key)) continue;

        for (const mode of HOLDING_MODES) {
          const settings = track.getSettings() as Record<string, unknown>;
          if (holding(settings, key)) break;
          await ask(constraintFor(key, mode, settings));
        }
        if (!holding(track.getSettings() as Record<string, unknown>, key)) hopeless.add(key);
      }

      // Read back rather than trust. `applyConstraints` resolving says the browser accepted the
      // request, not that the mode changed — and on the cameras where it does not, the burst
      // above would compare candidates on sharpness while the exposure moved under it.
      const settled = track.getSettings() as Record<string, unknown>;
      return ok<LockState>({
        exposure: holding(settled, 'exposure'),
        whiteBalance: holding(settled, 'whiteBalance'),
        focus: holding(settled, 'focus'),
      });
    },

    async close() {
      // Every track, not just video: a stream left running keeps the camera indicator lit, which
      // users reasonably read as the app spying on them.
      active?.getTracks().forEach((track) => track.stop());
      active = null;
      hopeless = new Set();
      offered = NOTHING_REPORTED;
      return ok(undefined);
    },
  };
}

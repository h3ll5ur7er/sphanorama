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
  close(): Promise<Result<void>>;
}

/** A capability array offers a lock when it contains a manual mode; absent means no. */
function offersManual(modes: unknown): boolean {
  return Array.isArray(modes) && modes.includes('manual');
}

const MODE_OF: Record<keyof LockState, string> = {
  exposure: 'exposureMode',
  whiteBalance: 'whiteBalanceMode',
  focus: 'focusMode',
};

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
        active = stream;

        // Report what the track settled on, not what we asked for: requested and granted
        // resolution differ constantly across devices, and the coverage plan is sized from the
        // real field of view.
        const track = stream.getVideoTracks()[0];
        const settings = track?.getSettings() ?? {};
        const capabilities = (track?.getCapabilities?.() ?? {}) as Record<string, unknown>;
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

    async setLocks(wanted: LockState) {
      const track = active?.getVideoTracks()[0] as (MediaStreamTrack & {
        applyConstraints?(constraints: unknown): Promise<void>;
      }) | undefined;
      if (!track) {
        return err<LockState>('CameraUnavailable', COMPONENT, 'no camera open');
      }

      const advanced: Record<string, string> = {};
      for (const key of Object.keys(MODE_OF) as (keyof LockState)[]) {
        advanced[MODE_OF[key]] = wanted[key] ? 'manual' : 'continuous';
      }

      try {
        await track.applyConstraints?.({ advanced: [advanced] });
      } catch {
        // A refusal is not a failure of this call: it means the camera would not take the locks,
        // which is exactly what the returned state is for. Reporting it as an error would make
        // the caller choose between no burst and a burst it knows nothing about.
      }

      // Read back rather than trust. `applyConstraints` resolving says the browser accepted the
      // request, not that the mode changed — and on the cameras where it does not, the burst
      // above would compare candidates on sharpness while the exposure moved under it.
      const settled = track.getSettings() as Record<string, unknown>;
      return ok<LockState>({
        exposure: settled[MODE_OF.exposure] === 'manual',
        whiteBalance: settled[MODE_OF.whiteBalance] === 'manual',
        focus: settled[MODE_OF.focus] === 'manual',
      });
    },

    async close() {
      // Every track, not just video: a stream left running keeps the camera indicator lit, which
      // users reasonably read as the app spying on them.
      active?.getTracks().forEach((track) => track.stop());
      active = null;
      return ok(undefined);
    },
  };
}

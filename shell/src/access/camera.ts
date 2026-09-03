/**
 * Browser implementation of ICameraAccess — the open/preview part of it.
 *
 * Burst capture is not here yet: it needs the frame store and the shared heap, and writing it
 * before those exist would mean inventing a second path for pixels.
 *
 * The mapping from getUserMedia's error names onto StatusCodes is the substance of this file. A
 * user who declined and a device with no camera need different words on screen, and the
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
}

export interface CameraAccess {
  open(spec: CameraOpenSpec): Promise<Result<CameraCapabilities>>;
  stream(): MediaStream | null;
  close(): Promise<Result<void>>;
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

  return {
    async open(spec: CameraOpenSpec) {
      if (!media?.getUserMedia) {
        // No mediaDevices means an insecure origin far more often than a device without a camera.
        return err<CameraCapabilities>('Unsupported', COMPONENT,
          'no media devices — the page must be served over https');
      }

      try {
        const stream = await media.getUserMedia({
          video: {
            facingMode: spec.preferRearCamera ? { ideal: 'environment' } : { ideal: 'user' },
            width: spec.preferredWidth ? { ideal: spec.preferredWidth } : undefined,
            height: spec.preferredHeight ? { ideal: spec.preferredHeight } : undefined,
          },
          audio: false,
        });
        active = stream;

        // Report what the track settled on, not what we asked for: requested and granted
        // resolution differ constantly across devices, and the coverage plan is sized from the
        // real field of view.
        const track = stream.getVideoTracks()[0];
        const settings = track?.getSettings() ?? {};
        const capabilities = track?.getCapabilities?.() ?? {};
        return ok({
          maxWidth: settings.width ?? 0,
          maxHeight: settings.height ?? 0,
          supportsTorch: 'torch' in capabilities,
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

    async close() {
      // Every track, not just video: a stream left running keeps the camera indicator lit, which
      // users reasonably read as the app spying on them.
      active?.getTracks().forEach((track) => track.stop());
      active = null;
      return ok(undefined);
    },
  };
}

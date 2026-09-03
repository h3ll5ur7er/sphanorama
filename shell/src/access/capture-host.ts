/**
 * The page half of the camera and motion ports.
 *
 * The core reads these synchronously, so everything here is state the page established earlier —
 * it opened the camera and asked for motion permission, both asynchronously, before the core was
 * ever asked to begin a session (ADR 0014).
 */

import type { ImuSample } from '../../../contracts/ts/contracts';

export interface CameraCapabilities {
  maxWidth: number;
  maxHeight: number;
  horizontalFovDeg: number;
  verticalFovDeg: number;
  supportsTorch: boolean;
}

/**
 * How many doubles one ImuSample occupies on the way to the core, and in what order.
 *
 * Flat doubles rather than the generated codec: the codec exists for the facade, and reaching for
 * it inside a resource-access port would put a second marshalling path in the layer whose whole
 * job is to keep platform shapes out of the core. The C++ side reads the same order, and the two
 * are pinned together by a test on each side.
 */
export const MOTION_SAMPLE_DOUBLES = 16;

/**
 * The most samples kept while the core is not draining. A capture loop that stalls must not be
 * able to kill the tab, and an orientation from several seconds ago is worthless anyway — so the
 * oldest go first.
 */
export const MOTION_BUFFER_LIMIT = 512;

export interface CaptureHost {
  cameraOpen(): boolean;
  cameraCapabilities(): CameraCapabilities;
  setCamera(camera: { maxWidth: number; maxHeight: number; supportsTorch: boolean }): void;
  clearCamera(): void;

  motionCapability(): string;
  setMotion(capability: string): void;

  /** Called by the page as orientation events arrive; nothing here awaits anything. */
  pushMotion(samples: ImuSample[]): void;
  /** Called synchronously from C++, through IMotionSensorAccess::Drain. */
  motionDrain(maxSamples: number): number[];
  /** Called from C++ on Stop: samples from a finished session must not seed the next one. */
  resetMotion(): void;

  /**
   * Stops the camera for real. Called synchronously from ICameraAccess::Close, so it must not
   * await anything — stopping a MediaStreamTrack is synchronous, which is what makes this
   * possible at all.
   */
  closeCamera(): void;
  /** Where the page hands over the stream it opened, so the host can stop it later. */
  setCameraStream(stream: MediaStream | null): void;
}

/**
 * A rear phone camera, roughly. The browser does not report field of view at all, and the
 * coverage plan cannot be built without one.
 *
 * This is a placeholder with a real successor: Phase 2's bundle adjustment estimates focal length
 * from the captured frames, which is the only way to actually know. Until then the plan is built
 * on an assumption, and a wrong assumption shows up as cells that overlap more or less than
 * intended rather than as a failure.
 */
const ASSUMED_HORIZONTAL_FOV_DEG = 66;
const FALLBACK_ASPECT = 4 / 3;

export function deriveFieldOfView(width: number, height: number, horizontalFovDeg: number) {
  // The resolution the track settled on is genuine even though the angle is an assumption, so the
  // aspect ratio is worth using rather than assuming that too.
  const aspect = width > 0 && height > 0 ? height / width : 1 / FALLBACK_ASPECT;
  const toRad = Math.PI / 180;
  // Angles do not scale linearly with sensor dimensions: scaling the tangent is the relationship
  // that actually holds, and treating it as linear would overstate the vertical angle and leave
  // gaps between rings.
  const verticalFovDeg = (2 * Math.atan(Math.tan((horizontalFovDeg * toRad) / 2) * aspect)) / toRad;
  return { horizontalFovDeg, verticalFovDeg };
}

function flatten(sample: ImuSample, into: number[]): void {
  into.push(
    sample.timestampNs,
    sample.angularVelocity.x, sample.angularVelocity.y, sample.angularVelocity.z,
    sample.acceleration.x, sample.acceleration.y, sample.acceleration.z,
    sample.hasMagnetometer ? 1 : 0,
    sample.magneticField.x, sample.magneticField.y, sample.magneticField.z,
    sample.hasOrientation ? 1 : 0,
    sample.orientation.w, sample.orientation.x, sample.orientation.y, sample.orientation.z,
  );
}

export function createCaptureHost(): CaptureHost {
  let camera: CameraCapabilities | null = null;
  let motion = 'None';
  let buffered: ImuSample[] = [];
  let stream: MediaStream | null = null;

  return {
    cameraOpen: () => camera !== null,

    cameraCapabilities(): CameraCapabilities {
      return camera ?? {
        maxWidth: 0, maxHeight: 0, horizontalFovDeg: 0, verticalFovDeg: 0, supportsTorch: false,
      };
    },

    setCamera(opened) {
      camera = {
        ...opened,
        ...deriveFieldOfView(opened.maxWidth, opened.maxHeight, ASSUMED_HORIZONTAL_FOV_DEG),
      };
    },

    clearCamera() {
      // A stale capability set would let the core plan a capture against a camera that is gone.
      camera = null;
    },

    setCameraStream(opened: MediaStream | null) {
      stream = opened;
    },

    closeCamera() {
      // Every track, not just video: a stream left running keeps the camera indicator lit, which
      // users reasonably read as the app still watching them after they ended a capture.
      stream?.getTracks().forEach((track) => track.stop());
      stream = null;
      camera = null;
    },

    motionCapability: () => motion,
    setMotion(capability: string) {
      motion = capability;
    },

    pushMotion(samples: ImuSample[]) {
      buffered.push(...samples);
      if (buffered.length > MOTION_BUFFER_LIMIT) {
        buffered = buffered.slice(buffered.length - MOTION_BUFFER_LIMIT);
      }
    },

    motionDrain(maxSamples: number): number[] {
      const taken = buffered.splice(0, Math.max(0, maxSamples));
      const flat: number[] = [];
      for (const sample of taken) flatten(sample, flat);
      return flat;
    },

    resetMotion() {
      buffered = [];
    },
  };
}

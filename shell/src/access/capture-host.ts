/**
 * The resident half of the camera and motion ports, in the context the core runs in.
 *
 * The core reads these synchronously, so everything here is state established earlier — the page
 * opened the camera and asked for motion permission, both asynchronously, before the core was
 * ever asked to begin a session (ADR 0014). Since the core moved into a worker (ADR 0019) that
 * earlier work happens on the other side of a `postMessage`, and this host is what the page keeps
 * fed. Nothing about the shape changed: it is still state that is already there by the time C++
 * asks for it.
 *
 * Two things could not come across. A `MediaStream` cannot be stopped from here, so closing is a
 * callback the composition root fills in; and samples arrive as flat doubles rather than
 * `ImuSample` objects, because that is what transfers to a worker without a copy.
 */

import type { LockState } from './camera';
import type { GrabbedFrame } from './preview-frame';
import type { ImuSample } from '../../../contracts/ts/contracts';

export interface CameraCapabilities {
  maxWidth: number;
  maxHeight: number;
  horizontalFovDeg: number;
  verticalFovDeg: number;
  supportsTorch: boolean;
  supportsExposureLock: boolean;
  supportsWhiteBalanceLock: boolean;
  supportsFocusLock: boolean;
}

/** Nothing locked — what a host with no camera reports, and what closing one goes back to. */
const NO_LOCKS: LockState = { exposure: false, whiteBalance: false, focus: false };

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
  setCamera(camera: Omit<CameraCapabilities, 'horizontalFovDeg' | 'verticalFovDeg'>): void;
  clearCamera(): void;

  /**
   * Which locks the page confirmed are actually held (ADR 0022).
   *
   * Read synchronously from `ICameraAccess::SetLocks`, which is why it is state rather than a
   * call: applying a lock is `applyConstraints`, which is asynchronous and refusable, and a
   * synchronous port has nothing to wait with. The page applies and confirms before a burst is
   * armed; this is what the core reads back.
   */
  cameraLocks(): LockState;
  setCameraLocks(locks: LockState): void;
  /**
   * Called synchronously from `ICameraAccess::SetLocks` when the core wants the locks back.
   *
   * Like `closeCamera`, all it can do is ask: the track is on the page. It clears the state here
   * rather than waiting to be told, because the core has asked and a port still reporting the
   * lock held would let the next burst arm on a confirmation that is no longer true.
   */
  releaseCameraLocks(): void;

  /**
   * The latest frame the page grabbed, or null when there is not one (ADR 0021).
   *
   * Read synchronously from C++ through `ICameraAccess::PeekPreviewFrame`, which copies it into
   * the frame store. Null is an answer rather than a wait — the port is synchronous and has
   * nothing to wait with — so a burst that peeks before the first frame arrives is told so.
   */
  previewFrame(): GrabbedFrame | null;
  /** Replaces whatever was there. Latest, not next: see the note on the field. */
  setPreviewFrame(frame: GrabbedFrame): void;

  motionCapability(): string;
  setMotion(capability: string): void;

  /** Flat doubles, as `flattenImuSamples` produces them. Nothing here awaits anything. */
  pushMotion(doubles: ArrayLike<number>): void;
  /** Called synchronously from C++, through IMotionSensorAccess::Drain. */
  motionDrain(maxSamples: number): number[];
  /** Called from C++ on Stop: samples from a finished session must not seed the next one. */
  resetMotion(): void;

  /**
   * Called synchronously from ICameraAccess::Close. It forgets the capabilities itself and asks
   * whoever holds the stream to stop it — which is the page, since a MediaStream cannot cross to
   * a worker. Asking is all it can do, and that is the same trade every write-only port call
   * takes (ADR 0019).
   */
  closeCamera(): void;
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

/**
 * Samples as the flat doubles the core reads, in one buffer that transfers without a copy.
 *
 * Done on the page's side of the boundary because that is where the samples are, and a
 * Float64Array handed over by transfer costs the same at any length — turning each sample into an
 * object graph and structured-cloning the array would pay per sample instead.
 */
export function flattenImuSamples(samples: ImuSample[]): Float64Array {
  const flat = new Float64Array(samples.length * MOTION_SAMPLE_DOUBLES);
  samples.forEach((sample, index) => {
    flat.set([
      sample.timestampNs,
      sample.angularVelocity.x, sample.angularVelocity.y, sample.angularVelocity.z,
      sample.acceleration.x, sample.acceleration.y, sample.acceleration.z,
      sample.hasMagnetometer ? 1 : 0,
      sample.magneticField.x, sample.magneticField.y, sample.magneticField.z,
      sample.hasOrientation ? 1 : 0,
      sample.orientation.w, sample.orientation.x, sample.orientation.y, sample.orientation.z,
    ], index * MOTION_SAMPLE_DOUBLES);
  });
  return flat;
}

/**
 * Stops a camera stream, every track of it.
 *
 * Video alone is not enough: any live track keeps the camera indicator lit, and a user who ended
 * a capture reasonably reads that as the app still watching them. It lives here beside the port
 * whose Close asks for it, and runs on the page, which is the only side that can hold a stream.
 */
export function stopCameraStream(stream: MediaStream | null): void {
  stream?.getTracks().forEach((track) => track.stop());
}

export interface CaptureHostOptions {
  /**
   * Stops the camera the page opened. Absent means nobody is holding a stream — which is the
   * native and test case, and is not a failure.
   */
  onCloseCamera?: () => void;
  /**
   * Puts the camera's locks back. Absent means nobody is holding a track — the native and test
   * case, and not a failure.
   */
  onReleaseCameraLocks?: () => void;
}

export function createCaptureHost(options: CaptureHostOptions = {}): CaptureHost {
  let camera: CameraCapabilities | null = null;
  // The latest grabbed frame, replaced rather than queued. A queue would hand a burst frames
  // from before the phone was aimed; the manager decides *when* to take one, from the clock.
  let preview: GrabbedFrame | null = null;
  // A lock belongs to a track, so this dies with the camera: reporting one held after the stream
  // is gone would let the next session arm a burst believing an exposure was fixed by a camera
  // that no longer exists.
  let locks: LockState = NO_LOCKS;
  let motion = 'None';
  // Doubles rather than samples, so a push is a copy of numbers into a numbers array and the
  // drain that C++ makes is a slice. The limit is still counted in samples, because that is the
  // unit the reason for it is expressed in.
  let buffered: number[] = [];

  return {
    cameraOpen: () => camera !== null,

    cameraCapabilities(): CameraCapabilities {
      return camera ?? {
        maxWidth: 0, maxHeight: 0, horizontalFovDeg: 0, verticalFovDeg: 0, supportsTorch: false,
        supportsExposureLock: false, supportsWhiteBalanceLock: false, supportsFocusLock: false,
      };
    },

    cameraLocks: () => locks,
    setCameraLocks(next: LockState) {
      locks = next;
    },

    releaseCameraLocks() {
      locks = NO_LOCKS;
      options.onReleaseCameraLocks?.();
    },

    setCamera(opened) {
      camera = {
        ...opened,
        ...deriveFieldOfView(opened.maxWidth, opened.maxHeight, ASSUMED_HORIZONTAL_FOV_DEG),
      };
    },

    clearCamera() {
      // A stale capability set would let the core plan a capture against a camera that is gone,
      // and a stale frame would let a burst capture the last thing the old stream saw — a
      // picture of somewhere the phone is no longer pointing.
      camera = null;
      preview = null;
      locks = NO_LOCKS;
    },

    closeCamera() {
      camera = null;
      preview = null;
      locks = NO_LOCKS;
      options.onCloseCamera?.();
    },

    previewFrame: () => preview,

    setPreviewFrame(frame: GrabbedFrame) {
      // The core sizes its copy from width and height, so a buffer that does not match them
      // would have it read past the end of memory the page transferred over. Every other failure
      // in this path is a bad picture; this one is not.
      preview = frame.bytes.length === frame.width * frame.height * 4 ? frame : null;
    },

    motionCapability: () => motion,
    setMotion(capability: string) {
      motion = capability;
    },

    pushMotion(doubles: ArrayLike<number>) {
      for (let i = 0; i < doubles.length; i++) buffered.push(doubles[i]);
      const ceiling = MOTION_BUFFER_LIMIT * MOTION_SAMPLE_DOUBLES;
      if (buffered.length > ceiling) buffered = buffered.slice(buffered.length - ceiling);
    },

    motionDrain(maxSamples: number): number[] {
      return buffered.splice(0, Math.max(0, maxSamples) * MOTION_SAMPLE_DOUBLES);
    },

    resetMotion() {
      buffered = [];
    },
  };
}

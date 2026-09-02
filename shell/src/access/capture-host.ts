/**
 * The page half of the camera and motion ports.
 *
 * The core reads these synchronously, so everything here is state the page established earlier —
 * it opened the camera and asked for motion permission, both asynchronously, before the core was
 * ever asked to begin a session (ADR 0014).
 */

export interface CameraCapabilities {
  maxWidth: number;
  maxHeight: number;
  horizontalFovDeg: number;
  verticalFovDeg: number;
  supportsTorch: boolean;
}

export interface CaptureHost {
  cameraOpen(): boolean;
  cameraCapabilities(): CameraCapabilities;
  setCamera(camera: { maxWidth: number; maxHeight: number; supportsTorch: boolean }): void;
  clearCamera(): void;

  motionCapability(): string;
  setMotion(capability: string): void;
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

export function createCaptureHost(): CaptureHost {
  let camera: CameraCapabilities | null = null;
  let motion = 'None';

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

    motionCapability: () => motion,
    setMotion(capability: string) {
      motion = capability;
    },
  };
}

/**
 * Every message that crosses between the page and the worker the core runs in (ADR 0019).
 *
 * One file, imported by both sides, because this is the crossing that is *not* generated. The
 * facade's is: a method id and a byte array in, a byte array out, knowing nothing about names, so
 * it cannot drift. This one carries the host — camera capabilities, IMU batches and the latest
 * preview frame — and a hand written protocol is exactly what ADR 0009 built a generator to avoid
 * for the other boundary.
 * Keeping it in one file that both sides import is the cheap half of that discipline; if it grows
 * past a handful of messages it wants the expensive half too.
 *
 * Requests that expect an answer carry a `seq`. Pushes do not, because the core reads them from
 * resident state whenever it next looks and there is nothing to wait for (ADR 0014).
 */
import type { RuntimeCapabilities } from './core';

export interface CameraOpening {
  maxWidth: number;
  maxHeight: number;
  supportsTorch: boolean;
  supportsExposureLock: boolean;
  supportsWhiteBalanceLock: boolean;
  supportsFocusLock: boolean;
}

/** Which locks the page confirmed are held, read back off the track rather than assumed. */
export interface LockReport {
  exposure: boolean;
  whiteBalance: boolean;
  focus: boolean;
}

export type ToWorker =
  /** The core module is fetched at runtime, so the page passes the URL it resolved. */
  | { kind: 'boot'; seq: number; coreUrl: string }
  | { kind: 'call'; seq: number; method: string; args: Uint8Array }
  | {
      kind: 'capabilities';
      seq: number;
      hardwareConcurrency: number;
      crossOriginIsolated: boolean;
    }
  /** Awaits the document host's pending writes. The client calls it at session end and on unload. */
  | { kind: 'flush'; seq: number }
  | { kind: 'camera'; opened: CameraOpening | null }
  | { kind: 'motion'; capability: string }
  /** Flat doubles, `MOTION_SAMPLE_DOUBLES` per sample, transferred rather than copied. */
  | { kind: 'imu'; doubles: Float64Array }
  /**
   * The latest frame the page grabbed: RGBA8, tightly packed, transferred (ADR 0021).
   *
   * The dimensions travel with it because the bytes alone do not say the shape, and the worker
   * side is what sizes the copy into the frame store from them. Megabytes per message, which is
   * why it is a transfer and why the page only sends one when a burst can use it.
   */
  | { kind: 'frame'; width: number; height: number; bytes: Uint8Array }
  /**
   * What the camera's locks actually settled on (ADR 0022).
   *
   * A push rather than a reply, because the core reads it as resident state: `SetLocks` is
   * synchronous and has nothing to wait with, so the page applies and confirms *before* arming
   * and this is what the port reads back.
   */
  | { kind: 'locks'; held: LockReport };

export type FromWorker =
  // `spill` says whether the worker got an OPFS handle. It is not a capability the core reads —
  // the composition root inside the module decides that from the same fact — but a page that
  // cannot spill will hit a ceiling early, and the client has to be able to say so rather than
  // present it as a mysterious refusal (ADR 0020).
  | { kind: 'booted'; seq: number; methods: string[]; spill: boolean }
  | { kind: 'result'; seq: number; bytes: Uint8Array }
  | { kind: 'capabilities'; seq: number; value: RuntimeCapabilities }
  | { kind: 'flushed'; seq: number; persistError: string | null }
  /**
   * Anything that threw where a reply was expected. Startup is three failures now — the worker
   * boots, the module loads inside it, the store opens — so the detail travels rather than being
   * flattened into one message the client cannot act on.
   */
  | { kind: 'failed'; seq: number; detail: string }
  /** `ICameraAccess::Close` reached the host. Only the page can stop a MediaStream. */
  | { kind: 'closeCamera' }
  /** The core wants the camera's locks back. Only the page can apply constraints to a track. */
  | { kind: 'releaseLocks' };

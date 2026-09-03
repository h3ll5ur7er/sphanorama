/**
 * Every message that crosses between the page and the worker the core runs in (ADR 0019).
 *
 * One file, imported by both sides, because this is the crossing that is *not* generated. The
 * facade's is: a method id and a byte array in, a byte array out, knowing nothing about names, so
 * it cannot drift. This one carries the host — capabilities, frames, IMU batches — and a hand
 * written protocol is exactly what ADR 0009 built a generator to avoid for the other boundary.
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
  | { kind: 'imu'; doubles: Float64Array };

export type FromWorker =
  | { kind: 'booted'; seq: number; methods: string[] }
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
  | { kind: 'closeCamera' };

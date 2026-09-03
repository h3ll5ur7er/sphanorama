/**
 * The Result shape the core uses, on the JavaScript side.
 *
 * Mirrors contracts/ts/contracts.d.ts rather than re-deriving it: adapters are implementations of
 * C++ contracts, and a failure has to arrive at the core as a StatusCode it can branch on, not as
 * a thrown Error the boundary would have to catch and guess about (ADR 0006).
 */
import type { Result, Status, StatusCode } from '../../../contracts/ts/contracts';

export type { Result, Status, StatusCode };

export function ok<T>(value: T): Result<T> {
  return { ok: true, value };
}

export function err<T>(code: StatusCode, component: string, detail = ''): Result<T> {
  return { ok: false, status: { code, component, detail } };
}

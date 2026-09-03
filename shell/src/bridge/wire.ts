/**
 * Wire primitives for the generated boundary codec — the TypeScript half of
 * `contracts/cpp/sphanorama/wire.h`.
 *
 * Hand-written for the same reason the C++ half is: this is the small layer where bounds checks
 * either exist or do not. Everything above it is generated from the contracts so the two sides
 * cannot disagree (ADR 0013).
 *
 * Little-endian throughout, matching the C++ side and WebAssembly's own memory layout.
 */

const UTF8 = { encoder: new TextEncoder(), decoder: new TextDecoder() };

export class Writer {
  private parts: Uint8Array[] = [];
  private length = 0;

  bool(value: boolean): void {
    this.push(new Uint8Array([value ? 1 : 0]));
  }

  i32(value: number): void {
    const buffer = new ArrayBuffer(4);
    new DataView(buffer).setInt32(0, value | 0, true);
    this.push(new Uint8Array(buffer));
  }

  f64(value: number): void {
    const buffer = new ArrayBuffer(8);
    new DataView(buffer).setFloat64(0, value, true);
    this.push(new Uint8Array(buffer));
  }

  u64(value: bigint): void {
    const buffer = new ArrayBuffer(8);
    new DataView(buffer).setBigUint64(0, value, true);
    this.push(new Uint8Array(buffer));
  }

  string(value: string): void {
    const encoded = UTF8.encoder.encode(value);
    this.i32(encoded.length);
    this.push(encoded);
  }

  bytes(value: Uint8Array): void {
    this.i32(value.length);
    this.push(value);
  }

  count(value: number): void {
    this.i32(value);
  }

  finish(): Uint8Array {
    const out = new Uint8Array(this.length);
    let at = 0;
    for (const part of this.parts) {
      out.set(part, at);
      at += part.length;
    }
    return out;
  }

  private push(part: Uint8Array): void {
    this.parts.push(part);
    this.length += part.length;
  }
}

/**
 * Never throws past the end and never resynchronises onto garbage: once a read runs out of
 * payload the reader is failed, and every later read returns a zero value. Callers check `ok`
 * once at the end, mirroring the C++ side.
 */
export class Reader {
  private view: DataView;
  private at = 0;
  private failed = false;

  constructor(private readonly data: Uint8Array) {
    this.view = new DataView(data.buffer, data.byteOffset, data.byteLength);
  }

  get ok(): boolean {
    return !this.failed;
  }

  bool(): boolean {
    if (!this.take(1)) return false;
    return this.view.getUint8(this.at - 1) !== 0;
  }

  i32(): number {
    if (!this.take(4)) return 0;
    return this.view.getInt32(this.at - 4, true);
  }

  f64(): number {
    if (!this.take(8)) return 0;
    return this.view.getFloat64(this.at - 8, true);
  }

  u64(): bigint {
    if (!this.take(8)) return 0n;
    return this.view.getBigUint64(this.at - 8, true);
  }

  string(): string {
    const bytes = this.bytes();
    return UTF8.decoder.decode(bytes);
  }

  bytes(): Uint8Array {
    const length = this.i32();
    if (this.failed || length < 0 || !this.take(length)) {
      this.failed = true;
      return new Uint8Array(0);
    }
    return this.data.subarray(this.at - length, this.at);
  }

  count(): number {
    const value = this.i32();
    // One corrupt length prefix must not become a multi-gigabyte allocation: a count cannot
    // exceed the bytes that remain, since every element occupies at least one.
    if (this.failed || value < 0 || value > this.data.length - this.at) {
      this.failed = true;
      return 0;
    }
    return value;
  }

  private take(size: number): boolean {
    if (this.failed || this.at + size > this.data.length) {
      this.failed = true;
      return false;
    }
    this.at += size;
    return true;
  }
}

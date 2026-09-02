import { describe, expect, it } from 'vitest';

import { Reader, Writer } from './wire';

function roundTrip(write: (w: Writer) => void): Reader {
  const writer = new Writer();
  write(writer);
  return new Reader(writer.finish());
}

describe('wire primitives', () => {
  it('round-trips every scalar kind', () => {
    const reader = roundTrip((w) => {
      w.bool(true);
      w.i32(-42);
      w.u64(0xfeedfacedeadbeefn);
      w.f64(3.5);
      w.string('hello');
    });
    expect(reader.bool()).toBe(true);
    expect(reader.i32()).toBe(-42);
    expect(reader.u64()).toBe(0xfeedfacedeadbeefn);
    expect(reader.f64()).toBe(3.5);
    expect(reader.string()).toBe('hello');
    expect(reader.ok).toBe(true);
  });

  it('preserves every bit of a 64-bit hash', () => {
    // The build graph is keyed on content hashes; a double would drop the low bits, and a stale
    // stage would be reused with nothing failing.
    const hash = 0x0123456789abcdefn;
    expect(roundTrip((w) => w.u64(hash)).u64()).toBe(hash);
  });

  it('round-trips non-ASCII text', () => {
    expect(roundTrip((w) => w.string('sphère · 全景')).string()).toBe('sphère · 全景');
  });

  it('handles empty strings and byte payloads', () => {
    const reader = roundTrip((w) => {
      w.string('');
      w.bytes(new Uint8Array(0));
    });
    expect(reader.string()).toBe('');
    expect(reader.bytes().length).toBe(0);
    expect(reader.ok).toBe(true);
  });

  it('round-trips binary payloads', () => {
    const payload = new Uint8Array([0, 255, 7, 0, 128]);
    expect(Array.from(roundTrip((w) => w.bytes(payload)).bytes())).toEqual(Array.from(payload));
  });

  it('fails rather than reading past the end', () => {
    const reader = roundTrip((w) => w.i32(1));
    expect(reader.i32()).toBe(1);
    expect(reader.ok).toBe(true);
    reader.i32();
    expect(reader.ok).toBe(false);
  });

  it('rejects a length prefix that outruns the payload', () => {
    // What a truncated postMessage looks like from the reading side.
    const writer = new Writer();
    writer.i32(64);
    const reader = new Reader(writer.finish());
    expect(reader.string()).toBe('');
    expect(reader.ok).toBe(false);
  });

  it('rejects an absurd count before anything is allocated', () => {
    const writer = new Writer();
    writer.i32(1_000_000_000);
    const reader = new Reader(writer.finish());
    expect(reader.count()).toBe(0);
    expect(reader.ok).toBe(false);
  });

  it('stays failed once it has failed', () => {
    const reader = new Reader(new Uint8Array(0));
    reader.i32();
    expect(reader.ok).toBe(false);
    expect(reader.i32()).toBe(0);
    expect(reader.string()).toBe('');
    expect(reader.ok).toBe(false);
  });
});

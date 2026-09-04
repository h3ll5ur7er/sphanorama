import { describe, expect, it, vi } from 'vitest';

import { createSpillHost, openSpillFile, type SpillFile } from './spill-host';

/** A spill file in memory. The allocator is what is under test; the syscalls are the browser's. */
function fakeFile(options: { shortWrites?: boolean } = {}): SpillFile & { bytes(): Uint8Array } {
  let buffer = new Uint8Array(0);
  const grow = (size: number) => {
    if (size <= buffer.length) return;
    const next = new Uint8Array(size);
    next.set(buffer);
    buffer = next;
  };
  return {
    write(bytes, at) {
      if (options.shortWrites) return bytes.length - 1;
      grow(at + bytes.length);
      buffer.set(bytes, at);
      return bytes.length;
    },
    read(into, at) {
      const available = Math.max(0, Math.min(into.length, buffer.length - at));
      into.set(buffer.subarray(at, at + available));
      return available;
    },
    truncate(size) {
      grow(size);
      buffer = buffer.subarray(0, size);
    },
    close() {},
    bytes: () => buffer,
  };
}

const frame = (fill: number, length = 16) => new Uint8Array(length).fill(fill);

describe('the spill host', () => {
  it('reads back exactly what was written, for several frames at once', () => {
    // The whole job. A sink that returned the wrong frame's bytes would produce a panorama with
    // one cell from somewhere else in it, and nothing upstream could tell.
    const host = createSpillHost(fakeFile());
    expect(host.write(1, frame(0xa1))).toBe(true);
    expect(host.write(2, frame(0xb2))).toBe(true);

    const first = new Uint8Array(16);
    const second = new Uint8Array(16);
    expect(host.read(1, first)).toBe(true);
    expect(host.read(2, second)).toBe(true);
    expect(first.every((b) => b === 0xa1)).toBe(true);
    expect(second.every((b) => b === 0xb2)).toBe(true);
  });

  it('reuses the space a dropped frame gave back', () => {
    // One handle means one file, and a session that only ever appended would grow it by every
    // frame it ever spilled — hundreds of megabytes of holes on a device that is already short.
    const file = fakeFile();
    const host = createSpillHost(file);
    host.write(1, frame(0x01));
    host.write(2, frame(0x02));
    const grown = file.bytes().length;

    host.drop(1);
    expect(host.write(3, frame(0x03))).toBe(true);
    expect(file.bytes().length).toBe(grown);

    // And the reused slot holds the new frame, not a mix of the two.
    const out = new Uint8Array(16);
    expect(host.read(3, out)).toBe(true);
    expect(out.every((b) => b === 0x03)).toBe(true);
  });

  it('rewrites a frame in place rather than leaking its old slot', () => {
    // The store demotes, faults in and demotes the same frame again as memory pressure moves.
    // A fresh slot each time is the same unbounded growth, arriving more slowly.
    const file = fakeFile();
    const host = createSpillHost(file);
    host.write(1, frame(0x11));
    const grown = file.bytes().length;

    expect(host.write(1, frame(0x22))).toBe(true);
    expect(file.bytes().length).toBe(grown);
    const out = new Uint8Array(16);
    host.read(1, out);
    expect(out.every((b) => b === 0x22)).toBe(true);
  });

  it('moves a frame that changed size instead of writing past its slot', () => {
    // Nothing captures two sizes today, and a slot silently overrun is the kind of corruption
    // that shows up three components away.
    const host = createSpillHost(fakeFile());
    host.write(1, frame(0x33, 16));
    host.write(2, frame(0x44, 16));
    expect(host.write(1, frame(0x55, 32))).toBe(true);

    const moved = new Uint8Array(32);
    const untouched = new Uint8Array(16);
    expect(host.read(1, moved)).toBe(true);
    expect(host.read(2, untouched)).toBe(true);
    expect(moved.every((b) => b === 0x55)).toBe(true);
    expect(untouched.every((b) => b === 0x44)).toBe(true);
  });

  it('reports a short write rather than claiming the frame is safe', () => {
    // A quota that ran out mid-write is how this fails on a real device. The store keeps the
    // frame in the heap when the sink says no, and can do nothing at all if the sink says yes.
    const host = createSpillHost(fakeFile({ shortWrites: true }));
    expect(host.write(1, frame(0x66))).toBe(false);
    expect(host.read(1, new Uint8Array(16))).toBe(false);
  });

  it('refuses to read a frame it never wrote, and one it has dropped', () => {
    const host = createSpillHost(fakeFile());
    expect(host.read(9, new Uint8Array(16))).toBe(false);
    host.write(9, frame(0x77));
    host.drop(9);
    expect(host.read(9, new Uint8Array(16))).toBe(false);
  });

  it('refuses a read of the wrong length rather than filling part of the buffer', () => {
    // The store always asks for the size it allocated, so this is a mismatch between the store
    // and the file — the one situation where handing back a plausible frame is worst.
    const host = createSpillHost(fakeFile());
    host.write(1, frame(0x88, 16));
    expect(host.read(1, new Uint8Array(8))).toBe(false);
    expect(host.read(1, new Uint8Array(32))).toBe(false);
  });

  it('forgets a dropped frame so its identity can be reused', () => {
    // Frame ids are unique within a session, but a resumed one starts counting again, and a
    // stale slot answering for a new frame is the same wrong-cell failure as above.
    const host = createSpillHost(fakeFile());
    host.write(4, frame(0x99));
    expect(host.drop(4)).toBe(true);
    expect(host.drop(4)).toBe(true); // idempotent: the caller wanted it gone and it is
    expect(host.read(4, new Uint8Array(16))).toBe(false);
  });
});

/**
 * An origin private file system with the one property that matters here: a sync access handle is
 * exclusive, so a second attempt to lock a file someone else holds throws.
 */
function fakeOpfs(initial: string[] = []) {
  const files = new Map(initial.map((name) => [name, { locked: false }]));
  return {
    names: () => [...files.keys()].sort(),
    directory: {
      async getFileHandle(name: string, options?: { create?: boolean }) {
        const existing = files.get(name);
        if (!existing && !options?.create) throw new Error(`no such file: ${name}`);
        const file = existing ?? { locked: false };
        files.set(name, file);
        return {
          async createSyncAccessHandle() {
            if (file.locked) throw new Error(`${name} is in use`);
            file.locked = true;
            return {
              write: () => 0,
              read: () => 0,
              truncate: () => {},
              close: () => {
                file.locked = false;
              },
            };
          },
        };
      },
      async removeEntry(name: string) {
        if (files.get(name)?.locked) throw new Error(`${name} is in use`);
        files.delete(name);
      },
      async *keys() {
        yield* [...files.keys()];
      },
    },
  };
}

describe('the spill file', () => {
  it('opens for a second session while the first still holds its own', async () => {
    // The bug this replaced: one fixed name and an exclusive handle, so the second tab open on
    // the app got no spill tier at all and captured a smaller sphere for no stated reason.
    const opfs = fakeOpfs();
    const first = await openSpillFile(opfs.directory);
    const second = await openSpillFile(opfs.directory);

    expect(first).not.toBe(second);
    expect(opfs.names()).toHaveLength(2);
  });

  it('opens anyway when a live session refuses to give its file up', async () => {
    // The sweep below asks for every spill file that is not this one, and a live sibling's file
    // says no. Letting that answer escape would cost the new session the tier it just opened —
    // the second tab's spill would be lost again, by the code meant to keep it.
    const opfs = fakeOpfs();
    await openSpillFile(opfs.directory);
    const held = opfs.names();

    await expect(openSpillFile(opfs.directory)).resolves.toBeDefined();
    expect(opfs.names()).toEqual(expect.arrayContaining(held));
  });

  it('reclaims the file of a session that is gone', async () => {
    // Nobody deletes these on a crash or a killed tab, and a unique name per session means they
    // would otherwise accumulate one abandoned spill file per run until the origin's quota went.
    const opfs = fakeOpfs(['sphanorama-spill-1a2b3c']);
    await openSpillFile(opfs.directory);

    expect(opfs.names()).not.toContain('sphanorama-spill-1a2b3c');
    expect(opfs.names()).toHaveLength(1);
  });

  it('leaves files that are not spill files alone', async () => {
    const opfs = fakeOpfs(['someone-elses-database']);
    await openSpillFile(opfs.directory);

    expect(opfs.names()).toContain('someone-elses-database');
  });

  it('removes its own file when the session closes', async () => {
    const opfs = fakeOpfs();
    const file = await openSpillFile(opfs.directory);
    expect(opfs.names()).toHaveLength(1);

    file.close();
    await vi.waitFor(() => expect(opfs.names()).toHaveLength(0));
  });
});

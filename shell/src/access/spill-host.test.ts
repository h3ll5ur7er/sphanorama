import { describe, expect, it, vi } from 'vitest';

import { createSpillHost, openSpillTier, type SpillFile } from './spill-host';

/** A spill file in memory. The allocator is what is under test; the syscalls are the browser's. */
function fakeFile(options: { shortWrites?: boolean } = {}):
    SpillFile & { bytes(): Uint8Array; failWrites(fail: boolean): void;
                  throwWrites(fail: boolean): void } {
  let buffer = new Uint8Array(0);
  let short = options.shortWrites === true;
  let throws = false;
  const grow = (size: number) => {
    if (size <= buffer.length) return;
    const next = new Uint8Array(size);
    next.set(buffer);
    buffer = next;
  };
  return {
    write(bytes, at) {
      // The other way a sync handle refuses: `write` is allowed to throw, and does when the
      // handle has gone away under it.
      if (throws) throw new Error('the handle is gone');
      if (short) {
        // Partially, the way a quota running out mid-write does: the bytes that fit are on disk.
        grow(at + bytes.length - 1);
        buffer.set(bytes.subarray(0, bytes.length - 1), at);
        return bytes.length - 1;
      }
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
    size: () => buffer.length,
    close() {},
    bytes: () => buffer,
    failWrites: (fail: boolean) => { short = fail; },
    throwWrites: (fail: boolean) => { throws = fail; },
  };
}

const frame = (fill: number, length = 16) => new Uint8Array(length).fill(fill);

describe('a spill tier that outlives the tab', () => {
  it('finds the frames the last session spilled', () => {
    // The reload. The core hands back the identities its session document named (ADR 0029) and
    // asks this for their bytes — but the map from identity to offset lived only in the closed
    // tab's memory, so every one of those frames was a file full of pixels nobody could find.
    const file = fakeFile();
    const index = fakeFile();

    const before = createSpillHost(file, index);
    before.write(1, frame(0xa1));
    before.write(2, frame(0xb2));
    before.close();

    const after = createSpillHost(file, index);
    const into = new Uint8Array(16);
    expect(after.read(2, into)).toBe(true);
    expect(into.every((b) => b === 0xb2)).toBe(true);
  });

  it('does not give a new frame space an old one is still in', () => {
    // The half that is easy to miss. Recovering the offsets and not the high-water mark leaves
    // the allocator handing out offset 0 again, so the resumed session's first spill lands on top
    // of a cell it just restored — and reads still succeed, with the wrong pixels.
    const file = fakeFile();
    const index = fakeFile();

    const before = createSpillHost(file, index);
    before.write(1, frame(0xa1));
    before.write(2, frame(0xb2));
    before.close();

    const after = createSpillHost(file, index);
    expect(after.write(3, frame(0xc3))).toBe(true);

    const first = new Uint8Array(16);
    const second = new Uint8Array(16);
    expect(after.read(1, first)).toBe(true);
    expect(after.read(2, second)).toBe(true);
    expect(first.every((b) => b === 0xa1)).toBe(true);
    expect(second.every((b) => b === 0xb2)).toBe(true);
  });

  it('remembers a frame that was dropped, and the hole it left', () => {
    // A drop is as much of the allocator's state as a write. Coming back believing a dropped
    // frame is still there would answer a read with whatever has since been written over it.
    const file = fakeFile();
    const index = fakeFile();

    const before = createSpillHost(file, index);
    before.write(1, frame(0xa1));
    before.write(2, frame(0xb2));
    before.drop(1);
    before.close();

    const after = createSpillHost(file, index);
    expect(after.read(1, new Uint8Array(16))).toBe(false);
    // And the hole is still on the free list, rather than the file growing past it.
    expect(after.write(3, frame(0xc3))).toBe(true);
    expect(file.bytes().length).toBe(32);
  });

  it('does not write over a frame the index named, even if it understates the file', () => {
    // The high-water mark and the slots are two statements about the same file, and only one of
    // them can be trusted: a slot says a frame is at an offset, which the next read will act on.
    // An `end` below the highest slot hands the next spill space a restored frame is sitting in,
    // and that read still succeeds — with the wrong pixels, which is the one failure here that
    // nothing downstream could trace back to this file.
    const file = fakeFile();
    const index = fakeFile();
    const before = createSpillHost(file, index);
    before.write(1, frame(0xa1));
    before.write(2, frame(0xb2));
    before.close();

    // The same index, with a high-water mark that has fallen behind what it describes.
    const stored = JSON.parse(new TextDecoder().decode(index.bytes()));
    const understated = new TextEncoder().encode(JSON.stringify({ ...stored, end: 0 }));
    index.truncate(understated.length);
    index.write(understated, 0);

    const after = createSpillHost(file, index);
    expect(after.write(3, frame(0xc3))).toBe(true);

    const first = new Uint8Array(16);
    expect(after.read(1, first)).toBe(true);
    expect(first.every((b) => b === 0xa1)).toBe(true);
  });

  it('leaves no index at all rather than half of one', () => {
    // A write that lands short is a phone out of quota mid-spill. The truncate before it means
    // what survives is a prefix, and no prefix of this document parses — so the next session
    // already starts empty. Emptying it outright is the difference between that being true by
    // construction and true by the shape of JSON.
    const file = fakeFile();
    const index = fakeFile({ shortWrites: true });

    const host = createSpillHost(file, index);
    expect(host.write(1, frame(0xa1))).toBe(true);
    expect(index.bytes().length).toBe(0);
  });

  it('does not allocate whatever the index file claims to be', () => {
    // The size comes off a file on disk, and this runs while the worker is booting. A corrupt
    // length there is a buffer the phone cannot allocate — the tier is a few kilobytes for a full
    // sphere, so anything on that scale is a broken file rather than a large one.
    let asked = 0;
    const enormous: SpillFile = {
      write: () => 0,
      read: (into) => { asked = Math.max(asked, into.length); return 0; },
      truncate: () => {},
      size: () => 8 * 1024 * 1024 * 1024,
      close: () => {},
    };

    const host = createSpillHost(fakeFile(), enormous);

    expect(asked).toBeLessThan(64 * 1024 * 1024);
    // And it is a working tier, just an empty one.
    expect(host.write(1, frame(0xa1))).toBe(true);
  });

  it('does not leave a reload pointing at a frame a failed rewrite half-destroyed', () => {
    // A rewrite of the same size lands in the same place — that is the free list working. So a
    // write that fails partway has already overwritten the frame that was there, and the code
    // handles it by forgetting the frame entirely. That was sound while the map lived only in
    // this process: nothing could ask for it again, because the store keeps a frame it could not
    // spill in the heap. A durable index is exactly something that asks again, so a failure has
    // to reach the index too — or the next session reads that slot and gets half of each frame,
    // and reports success.
    const file = fakeFile();
    const index = fakeFile();

    const before = createSpillHost(file, index);
    expect(before.write(1, frame(0xa1))).toBe(true);
    file.failWrites(true);
    expect(before.write(1, frame(0xb2))).toBe(false);
    before.close();

    const after = createSpillHost(file, index);
    expect(after.read(1, new Uint8Array(16))).toBe(false);
  });

  it('does the same when the rewrite threw rather than fell short', () => {
    // The same hole by the other door. A sync handle may throw instead of returning a short
    // count — the file went away under the worker — and a frame forgotten in memory but still
    // named on disk is the same corrupt read next session either way.
    const file = fakeFile();
    const index = fakeFile();

    const before = createSpillHost(file, index);
    expect(before.write(1, frame(0xa1))).toBe(true);
    file.throwWrites(true);
    expect(before.write(1, frame(0xb2))).toBe(false);
    before.close();

    const after = createSpillHost(file, index);
    expect(after.read(1, new Uint8Array(16))).toBe(false);
  });

  it('starts empty rather than throwing when the index makes no sense', () => {
    // It is a file on a phone: a tab killed mid-write, a quota that ran out between the frame and
    // the index. Losing the tier's memory costs a resume; throwing here costs the whole session,
    // because this runs while the worker is booting.
    const file = fakeFile();
    const index = fakeFile();
    index.write(new TextEncoder().encode('{ this is not an index'), 0);

    const host = createSpillHost(file, index);
    expect(host.read(1, new Uint8Array(16))).toBe(false);
    expect(host.write(1, frame(0xa1))).toBe(true);
  });

  it('works with no index at all, which is what a browser without one gets', () => {
    const host = createSpillHost(fakeFile());
    expect(host.write(1, frame(0xa1))).toBe(true);
    expect(host.read(1, new Uint8Array(16))).toBe(true);
  });
});

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
function fakeOpfs(initial: string[] = [], refuseLock: (name: string) => boolean = () => false,
                 failClose: (name: string) => boolean = () => false,
                 noSyncHandles = false) {
  const files = new Map(initial.map((name) => [name, { locked: false }]));
  return {
    names: () => [...files.keys()].sort(),
    directory: {
      async getFileHandle(name: string, options?: { create?: boolean }) {
        const existing = files.get(name);
        if (!existing && !options?.create) throw new Error(`no such file: ${name}`);
        const file = existing ?? { locked: false };
        files.set(name, file);
        // A browser with no synchronous access handles at all, which is a different thing from a
        // handle somebody else is holding: no amount of trying another name will help.
        if (noSyncHandles) return {};
        return {
          async createSyncAccessHandle() {
            if (file.locked || refuseLock(name)) throw new Error(`${name} is in use`);
            file.locked = true;
            return {
              write: () => 0,
              read: () => 0,
              truncate: () => {},
              getSize: () => 0,
              close: () => {
                file.locked = false;
                // A handle that throws on the way out. The spec says closing twice is a no-op,
                // but this is a browser API on a phone and the code around it already treats
                // every other call to it as fallible.
                if (failClose(name)) throw new Error(`${name} would not close`);
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
  it('comes back to the same file, so a reload can still find its frames', async () => {
    // The whole reason this file has a fixed name again. A session's frames are named by the
    // identities its document carries (ADR 0029), and a fresh file per run means those bytes are
    // in a file that was swept before anybody asked for them.
    const opfs = fakeOpfs();
    const first = await openSpillTier(opfs.directory);
    const names = opfs.names();
    first.frames.close();
    first.index.close();

    await openSpillTier(opfs.directory);
    expect(opfs.names().sort()).toEqual(names.sort());
  });

  it('opens for a second session while the first still holds its own', async () => {
    // The bug that made the name unique in the first place: one fixed name and an exclusive
    // handle, so the second tab open on the app got no spill tier at all and captured a smaller
    // sphere for no stated reason. It still gets one — just not the resumable one, which belongs
    // to whoever is holding it.
    const opfs = fakeOpfs();
    const first = await openSpillTier(opfs.directory);
    const second = await openSpillTier(opfs.directory);

    expect(first.frames).not.toBe(second.frames);
    expect(opfs.names().filter((name) => name.startsWith('sphanorama-spill-'))).toHaveLength(4);
  });

  it('opens anyway when a live session refuses to give its file up', async () => {
    // The sweep asks for every spill file that is neither this session's nor the resident one,
    // and a live sibling's says no. Letting that answer escape would cost the new session the
    // tier it just opened — the second tab's spill lost again, by the code meant to keep it.
    const opfs = fakeOpfs();
    await openSpillTier(opfs.directory);
    const held = opfs.names();

    await expect(openSpillTier(opfs.directory)).resolves.toBeDefined();
    expect(opfs.names()).toEqual(expect.arrayContaining(held));
  });

  it('reclaims the file of a session that is gone', async () => {
    // Nobody deletes a fallback file on a crash or a killed tab, and they would otherwise
    // accumulate one per second-tab run until the origin's quota went.
    const opfs = fakeOpfs(['sphanorama-spill-1a2b3c']);
    await openSpillTier(opfs.directory);

    expect(opfs.names()).not.toContain('sphanorama-spill-1a2b3c');
  });

  it('never reclaims the resident tier, even lying unlocked', async () => {
    // The narrow race, and the expensive one. A session that could not take the resident pair
    // falls back to a name of its own and then sweeps — and if the tab that was holding that pair
    // closed in between, the sweep meets it unlocked, which is what an abandoned file looks like.
    // It is not abandoned: it is somebody's finished capture, and the sweep is the one operation
    // here that can destroy one. Left alone whatever its lock says; clearing it is a new
    // capture's job, because only the core knows the difference between beginning and resuming.
    const opfs = fakeOpfs(
      ['sphanorama-spill-resident', 'sphanorama-spill-resident.index'],
      (name) => name.startsWith('sphanorama-spill-resident'),
    );

    const tier = await openSpillTier(opfs.directory);

    expect(opfs.names()).toContain('sphanorama-spill-resident');
    expect(opfs.names()).toContain('sphanorama-spill-resident.index');
    // And this session is on a tier of its own rather than sharing one.
    expect(opfs.names().filter((name) => name.startsWith('sphanorama-spill-'))).toHaveLength(4);
    tier.frames.close();
    tier.index.close();
  });

  it('still gets a tier when only the index of the resident pair is held', async () => {
    // Two files, two locks, and only one of them has to be unavailable. Giving up there would
    // cost the session its spill tier entirely — a sphere capped at RAM — for a file it does not
    // even keep pixels in. A tier of its own beats no tier at all, here as everywhere else.
    const opfs = fakeOpfs([], (name) => name === 'sphanorama-spill-resident.index');

    const tier = await openSpillTier(opfs.directory);

    expect(tier.frames).toBeDefined();
    expect(tier.index).toBeDefined();
    // On a name of its own, both halves of it, rather than half-sharing the resident pair.
    const fallbacks = opfs.names().filter((name) => !name.startsWith('sphanorama-spill-resident'));
    expect(fallbacks).toHaveLength(2);
  });

  it('gives a fallback file back even when the handle will not close', async () => {
    // The close and the unlink are two steps, and only the second one frees the disk. A throw
    // between them leaves the file behind for a sweep that may never come — nobody opens this app
    // twice a day — so the cleanup has to survive the handle it is cleaning up after.
    const opfs = fakeOpfs([], () => false, (name) => !name.startsWith('sphanorama-spill-resident'));
    await openSpillTier(opfs.directory);
    const fallback = await openSpillTier(opfs.directory);

    expect(() => {
      fallback.frames.close();
      fallback.index.close();
    }).not.toThrow();
    await vi.waitFor(() => expect(opfs.names()).toHaveLength(2));
  });

  it('still falls back when the resident frame handle will not let go', async () => {
    // The recovery path's own cleanup. Releasing the resident frames is what makes the fallback
    // honest — leaving that handle open would keep the resident file locked for the life of the
    // worker — but a throw there used to abandon the recovery altogether and cost the session its
    // spill tier, which is the outcome the fallback exists to prevent.
    const opfs = fakeOpfs(
      [],
      (name) => name === 'sphanorama-spill-resident.index',
      (name) => name === 'sphanorama-spill-resident',
    );

    const tier = await openSpillTier(opfs.directory);

    expect(tier.frames).toBeDefined();
    const fallbacks = opfs.names().filter((name) => !name.startsWith('sphanorama-spill-resident'));
    expect(fallbacks).toHaveLength(2);
  });

  it('does not litter the disk on a browser that has no sync handles at all', async () => {
    // The fallback exists for a tier somebody else is holding — try another name and you get one.
    // "This browser cannot do synchronous handles" is not that: no name will work, and taking a
    // fresh uuid each boot creates an empty file, fails again, and leaves it there. Nothing ever
    // collects it either, because the sweep only runs once a tier has been locked.
    const opfs = fakeOpfs([], () => false, () => false, true);

    await expect(openSpillTier(opfs.directory)).rejects.toThrow(/synchronous access handles/);
    expect(opfs.names().filter((name) => name !== 'sphanorama-spill-resident')).toHaveLength(0);
  });

  it('leaves files that are not spill files alone', async () => {
    const opfs = fakeOpfs(['someone-elses-database']);
    await openSpillTier(opfs.directory);

    expect(opfs.names()).toContain('someone-elses-database');
  });

  it('removes a fallback session\'s own files when it closes', async () => {
    // A second tab's tier is nobody's to resume — its frames were never written into a session
    // document anyone will read — so it goes when the tab does rather than waiting for a sweep.
    const opfs = fakeOpfs();
    const resident = await openSpillTier(opfs.directory);
    const fallback = await openSpillTier(opfs.directory);
    expect(opfs.names()).toHaveLength(4);

    fallback.frames.close();
    fallback.index.close();
    await vi.waitFor(() => expect(opfs.names()).toHaveLength(2));
    expect(resident.frames).toBeDefined();
  });
});

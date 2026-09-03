/**
 * Where a spilled frame's bytes go in the browser.
 *
 * The core's frame store decides *when* a frame leaves the heap and *whether* there is room to
 * bring it back; this decides only where the bytes sit in the meantime (ADR 0020). It is the page
 * — worker — half of `ISpillSink`, and it exists in a worker because `createSyncAccessHandle` is
 * worker-only and a fault-in on `Pin` has to be synchronous (ADR 0019).
 *
 * One handle, opened once and held for the session, which means one file and therefore an
 * allocator: frames get offsets inside it, and a dropped frame's slot goes on a free list for the
 * next frame of the same size. A capture's frames are all one size, so that free list is close to
 * a perfect fit in practice — and without it the file would grow by every frame the session ever
 * spilled rather than by the most it ever held at once.
 */

/**
 * The synchronous file underneath, kept separate so the allocator can be tested without OPFS.
 * The three methods are `FileSystemSyncAccessHandle`'s, with its options object flattened.
 */
export interface SpillFile {
  write(bytes: Uint8Array, at: number): number;
  read(into: Uint8Array, at: number): number;
  truncate(size: number): void;
  close(): void;
}

export interface SpillHost {
  write(frame: number, bytes: Uint8Array): boolean;
  read(frame: number, into: Uint8Array): boolean;
  drop(frame: number): boolean;
  close(): void;
}

interface Slot {
  offset: number;
  length: number;
}

export function createSpillHost(file: SpillFile): SpillHost {
  const slots = new Map<number, Slot>();
  // Keyed by exact length. Splitting a larger hole would need coalescing to stay useful, and
  // every frame in a capture is the same size — so exact-fit reuse is nearly total and the
  // general allocator would be complexity bought for a case that does not arise.
  const free = new Map<number, number[]>();
  let end = 0;

  const release = (slot: Slot): void => {
    const holes = free.get(slot.length);
    if (holes) holes.push(slot.offset);
    else free.set(slot.length, [slot.offset]);
  };

  const take = (length: number): number => {
    const holes = free.get(length);
    const reused = holes?.pop();
    if (reused !== undefined) return reused;
    const offset = end;
    end += length;
    return offset;
  };

  return {
    write(frame, bytes) {
      // The old slot goes back before the new one is taken, which is what makes a rewrite of the
      // same size land in the same place: the free list is exact-fit and LIFO, so the hole just
      // made is the one handed straight back. Special-casing it would be a branch that cannot be
      // told from this one from the outside. The store demotes, faults in and demotes the same
      // frame repeatedly as pressure moves, so growing the file per rewrite is not hypothetical.
      const existing = slots.get(frame);
      if (existing) {
        slots.delete(frame);
        release(existing);
      }
      const slot = { offset: take(bytes.length), length: bytes.length };
      try {
        if (file.write(bytes, slot.offset) !== bytes.length) {
          // A quota exhausted mid-write is how this fails on a real device. The slot goes back on
          // the free list and the frame stays unknown — including its previous copy, which the
          // failed write has already partly overwritten and which nothing would ever ask for
          // again anyway, because the store keeps a frame it could not spill in the heap.
          release(slot);
          return false;
        }
      } catch {
        release(slot);
        return false;
      }
      slots.set(frame, slot);
      return true;
    },

    read(frame, into) {
      const slot = slots.get(frame);
      if (slot === undefined) return false;
      // A length mismatch means the store and this file disagree about the frame, which is the
      // one case where handing back a plausible buffer is worse than refusing: a short read would
      // reach an engine as a frame that is partly the previous one.
      if (slot.length !== into.length) return false;
      try {
        return file.read(into, slot.offset) === into.length;
      } catch {
        return false;
      }
    },

    drop(frame) {
      const slot = slots.get(frame);
      // Idempotent by contract: a sink that has already lost the frame has the outcome the caller
      // wanted. Nothing is written over the bytes — the next frame in this slot overwrites them,
      // and zeroing on drop would cost a file write per discarded candidate.
      if (slot === undefined) return true;
      slots.delete(frame);
      release(slot);
      return true;
    },

    close() {
      try {
        file.close();
      } catch {
        // Nothing left to tell. The session is over and the handle is going away with the worker.
      }
    },
  };
}

/**
 * Opens the session's spill file, or reports why it could not.
 *
 * Truncated on open rather than resumed: spilled bytes are keyed by the store's frame identity,
 * which starts again at 1 in a new session, so yesterday's file would answer for today's frames.
 * Resuming a capture across a reload restores the *session document*, not the pixel heap.
 */
export async function openSpillFile(name = 'sphanorama-spill'): Promise<SpillFile> {
  const storage = (navigator as unknown as { storage?: StorageManager }).storage;
  if (!storage?.getDirectory) throw new Error('this browser has no origin private file system');

  const root = await storage.getDirectory();
  const handle = await root.getFileHandle(name, { create: true });
  // Typed here rather than relied on from lib.dom: sync access handles are worker-only and the
  // ambient definitions for them are not in every TypeScript release we build against, which is
  // exactly the kind of thing that turns into a red build on someone else's machine.
  interface SyncAccessHandle {
    write(bytes: Uint8Array, options: { at: number }): number;
    read(into: Uint8Array, options: { at: number }): number;
    truncate(size: number): void;
    close(): void;
  }
  const sync = await (
    handle as unknown as {
      createSyncAccessHandle?: () => Promise<SyncAccessHandle>;
    }
  ).createSyncAccessHandle?.();
  if (!sync) throw new Error('this browser has no synchronous access handles');

  sync.truncate(0);
  return {
    write: (bytes, at) => sync.write(bytes, { at }),
    read: (into, at) => sync.read(into, { at }),
    truncate: (size) => sync.truncate(size),
    close: () => sync.close(),
  };
}

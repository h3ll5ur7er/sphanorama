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
 * The pieces of `FileSystemSyncAccessHandle` this uses, typed here rather than relied on from
 * lib.dom: sync access handles are worker-only and the ambient definitions for them are not in
 * every TypeScript release we build against, which is exactly the kind of thing that turns into a
 * red build on someone else's machine.
 */
interface SyncAccessHandle {
  write(bytes: Uint8Array, options: { at: number }): number;
  read(into: Uint8Array, options: { at: number }): number;
  truncate(size: number): void;
  close(): void;
}

/** The directory the spill files live in — the subset of `FileSystemDirectoryHandle` this uses. */
export interface SpillDirectory {
  getFileHandle(
    name: string,
    options?: { create?: boolean },
  ): Promise<{ createSyncAccessHandle?: () => Promise<SyncAccessHandle> }>;
  removeEntry(name: string): Promise<void>;
  keys(): AsyncIterable<string>;
}

const SPILL_PREFIX = 'sphanorama-spill-';

/**
 * Takes back the spill files no session is holding.
 *
 * A live session's file protects itself: a sync access handle is an exclusive lock for as long as
 * it is open, and unlinking a locked entry fails. So this asks for every spill file that is not
 * ours and keeps the ones that refuse — which are exactly the ones still in use.
 *
 * Nothing else would ever delete these, and with a file per session they would otherwise pile up
 * one per run — a crashed tab, a killed one, a reload that outran the close below — until the
 * origin's quota ran out. Best-effort throughout: a file that will not enumerate or unlink stays
 * where it is, because the cost of leaving it is disk that stays used and the cost of giving up
 * is the whole tier.
 */
async function reclaimAbandoned(directory: SpillDirectory, keep: string): Promise<void> {
  const names: string[] = [];
  try {
    for await (const name of directory.keys()) names.push(name);
  } catch {
    // A directory that will not enumerate leaves nothing to reclaim. Ours is already open.
  }

  for (const name of names) {
    if (name === keep || !name.startsWith(SPILL_PREFIX)) continue;
    // Per file, so one live sibling does not stop the sweep reaching the dead ones behind it.
    await directory.removeEntry(name).catch(() => {});
  }
}

async function originPrivateDirectory(): Promise<SpillDirectory> {
  const storage = (navigator as unknown as { storage?: StorageManager }).storage;
  if (!storage?.getDirectory) throw new Error('this browser has no origin private file system');
  return (await storage.getDirectory()) as unknown as SpillDirectory;
}

/**
 * Opens this session's spill file, or reports why it could not.
 *
 * The name is unique per session rather than fixed, because the handle underneath is exclusive:
 * with one shared name, the second tab open on the app — or a reload whose previous worker has
 * not been torn down yet — gets `NoModificationAllowedError` and captures with no spill tier for
 * no reason it could state. A fresh file also settles what a stale one would have meant: spilled
 * bytes are keyed by the store's frame identity, which starts again at 1 in a new session, so
 * yesterday's file would answer for today's frames. Resuming a capture across a reload restores
 * the *session document*, not the pixel heap.
 */
export async function openSpillFile(directory?: SpillDirectory): Promise<SpillFile> {
  const root = directory ?? (await originPrivateDirectory());
  const name = SPILL_PREFIX + crypto.randomUUID();
  const handle = await root.getFileHandle(name, { create: true });
  const sync = await handle.createSyncAccessHandle?.();
  if (!sync) throw new Error('this browser has no synchronous access handles');

  // Swept only once ours is locked, so the sweep cannot reach the file this call is about to
  // take. Another session inside its own window between create and lock can still lose its file
  // to this; it falls to the no-spill path the caller already handles, and closing that window
  // would need a lock protocol of its own for a race two tabs must boot milliseconds apart to
  // reach.
  await reclaimAbandoned(root, name);

  return {
    write: (bytes, at) => sync.write(bytes, { at }),
    read: (into, at) => sync.read(into, { at }),
    truncate: (size) => sync.truncate(size),
    close: () => {
      sync.close();
      // Fire and forget: the store closes the sink synchronously, and a tab being torn down has
      // no time to await anything. Whatever this misses, the next session's sweep collects.
      void root.removeEntry(name).catch(() => {});
    },
  };
}

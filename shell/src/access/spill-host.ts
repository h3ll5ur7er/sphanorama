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
  /** What `FileSystemSyncAccessHandle.getSize` reports. The index needs it; the frame file does not. */
  size(): number;
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

/**
 * The allocator's own state, written beside the frames so a reload can find them.
 *
 * Which frame is at which offset lives only in this process, and the file it describes outlives
 * the process by design (ADR 0029): a resumed session hands the store back the identities its
 * session document named, and the store asks this sink for their bytes. Without the map that
 * request reaches a file full of pixels nobody can locate.
 *
 * A second file rather than a header inside the first, because the first is addressed by offset
 * and a header would move every frame in it. Rewritten whole on each change: at one entry per
 * spilled frame it is a few kilobytes against megabyte frames, and a journal would need its own
 * recovery — for state that is already recoverable by being written again.
 */
interface StoredIndex {
  v: number;
  end: number;
  slots: [number, number, number][];   // frame, offset, length
  free: [number, number[]][];          // length, offsets
}

const INDEX_VERSION = 1;

// What a plausible index cannot exceed. A full sphere is twenty-eight cells of five frames, and
// each entry is a few dozen bytes of JSON — kilobytes, not megabytes. The length comes off a file
// on disk and is read while the worker is booting, so a corrupt one is a buffer the phone cannot
// allocate; past this, the file is broken rather than large.
const INDEX_CEILING = 8 * 1024 * 1024;

function readIndex(index: SpillFile | undefined, slots: Map<number, Slot>,
                   free: Map<number, number[]>): number {
  if (!index) return 0;
  let text: string;
  try {
    const size = index.size();
    if (size <= 0 || size > INDEX_CEILING) return 0;
    const bytes = new Uint8Array(size);
    if (index.read(bytes, 0) !== size) return 0;
    text = new TextDecoder().decode(bytes);
  } catch {
    return 0;
  }

  // Anything unreadable starts the tier empty rather than throwing. This runs while the worker is
  // booting, and a file on a phone can be a tab killed mid-write or a quota that ran out between
  // the frame and the index — losing the map costs a resume, and throwing costs the session.
  try {
    const parsed = JSON.parse(text) as StoredIndex;
    if (parsed?.v !== INDEX_VERSION || !Array.isArray(parsed.slots)) return 0;
    if (typeof parsed.end !== 'number' || !Number.isFinite(parsed.end) || parsed.end < 0) return 0;
    const giveUp = () => { slots.clear(); free.clear(); return 0; };
    for (const entry of parsed.slots) {
      if (!Array.isArray(entry) || entry.length !== 3) return giveUp();
      const [frame, offset, length] = entry;
      if (!Number.isFinite(frame) || !Number.isFinite(offset) || !Number.isFinite(length)
          || offset < 0 || length <= 0) {
        return giveUp();
      }
      slots.set(frame, { offset, length });
    }

    // The holes come back too. They are recoverable from the slots in principle — a gap below the
    // high-water mark is free — but only for frames of one size: two dropped frames side by side
    // read as one gap of twice the length, which this allocator's exact-fit list would then offer
    // to nothing. Writing them down is shorter than the arithmetic and cannot disagree with it.
    if (Array.isArray(parsed.free)) {
      for (const entry of parsed.free) {
        if (!Array.isArray(entry) || entry.length !== 2 || !Array.isArray(entry[1])) {
          return giveUp();
        }
        const [length, offsets] = entry;
        if (!Number.isFinite(length) || length <= 0
            || !offsets.every((at) => Number.isFinite(at) && at >= 0)) {
          return giveUp();
        }
        free.set(length, [...offsets]);
      }
    }
    // The slots win where the two disagree. They are statements about the same file, but only a
    // slot is acted on — a read goes to the offset it names — so a high-water mark that has
    // fallen behind one would hand the next spill space a restored frame is sitting in, and that
    // read still succeeds, with the wrong pixels. Raising it costs the gap in the file and
    // nothing else.
    let end = parsed.end;
    for (const slot of slots.values()) end = Math.max(end, slot.offset + slot.length);
    return end;
  } catch {
    slots.clear();
    free.clear();
    return 0;
  }
}

export function createSpillHost(file: SpillFile, index?: SpillFile): SpillHost {
  const slots = new Map<number, Slot>();
  // Keyed by exact length. Splitting a larger hole would need coalescing to stay useful, and
  // every frame in a capture is the same size — so exact-fit reuse is nearly total and the
  // general allocator would be complexity bought for a case that does not arise.
  const free = new Map<number, number[]>();
  // The high-water mark comes back with the slots, and losing it is the subtler half: an
  // allocator that recovered the offsets and started `end` at zero would hand the resumed
  // session's first spill the space a restored frame is sitting in, and the read would still
  // succeed — with the wrong pixels.
  let end = readIndex(index, slots, free);

  const persist = (): void => {
    if (!index) return;
    const stored: StoredIndex = {
      v: INDEX_VERSION,
      end,
      slots: [...slots].map(([frame, slot]) => [frame, slot.offset, slot.length]),
      free: [...free].map(([length, offsets]) => [length, [...offsets]]),
    };
    try {
      const bytes = new TextEncoder().encode(JSON.stringify(stored));
      index.truncate(bytes.length);
      if (index.write(bytes, 0) !== bytes.length) {
        // A phone out of quota mid-spill. The truncate above means what survives is a prefix of
        // this document, and no prefix of it parses — so the next session would start empty
        // anyway. Emptying it outright is the difference between that being true by construction
        // and true because of the shape of JSON.
        index.truncate(0);
      }
    } catch {
      // The frames are written and readable in this session either way. What a failure here costs
      // is the next one's resume, and there is nobody on this path to tell: the store has already
      // been told its frame is safe, which it is.
    }
  };

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
          // the free list and the frame stays unknown — including its previous copy, which a
          // same-size rewrite has already partly overwritten, since the free list is exact-fit
          // and hands the same offset straight back.
          //
          // Written down as well, which is the part a durable index changes. Nothing in *this*
          // session would ask for that frame again — the store keeps one it could not spill in
          // the heap — but a reload asks, and an index still naming the old slot would answer it
          // out of bytes that are now half of each frame, and report success.
          release(slot);
          persist();
          return false;
        }
      } catch {
        release(slot);
        persist();
        return false;
      }
      slots.set(frame, slot);
      persist();
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
      persist();
      return true;
    },

    close() {
      try {
        index?.close();
      } catch {
        // Same as below: the session is over and the handle is going away with the worker.
      }
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
  getSize(): number;
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
// The one tier a reload comes back to. Fixed rather than unique, which is a reversal: a name per
// session was what stopped the second tab losing its spill entirely (ADR 0020), and a fixed one
// is what lets a session find the frames it left behind (ADR 0029). Both hold here because the
// fixed name is only the *preferred* one — a session that cannot have it still gets a tier.
const RESIDENT = SPILL_PREFIX + 'resident';
const INDEX_SUFFIX = '.index';

/** A session's spill tier: the frames, and the map that says where in them each frame is. */
export interface SpillTier {
  frames: SpillFile;
  index: SpillFile;
}

/**
 * Takes back the spill files no session is holding, except the resident pair.
 *
 * A live session's files protect themselves: a sync access handle is an exclusive lock for as
 * long as it is open, and unlinking a locked entry fails. So this asks for every spill file that
 * is neither ours nor resident, and keeps the ones that refuse — which are exactly the ones still
 * in use.
 *
 * The resident pair is exempt because it is *meant* to be lying around unlocked: that is what a
 * session that ended looks like, and it is the capture a reload comes back for. Clearing it is a
 * new capture's job rather than a sweep's, because only the core knows the difference between
 * beginning and resuming.
 *
 * Nothing else would ever delete the fallback files, and with a name per second-tab run they
 * would otherwise pile up — a crashed tab, a killed one, a reload that outran the close below —
 * until the origin's quota ran out. Best-effort throughout: a file that will not enumerate or
 * unlink stays where it is, because the cost of leaving it is disk that stays used and the cost
 * of giving up is the whole tier.
 */
async function reclaimAbandoned(directory: SpillDirectory, keep: string[]): Promise<void> {
  const names: string[] = [];
  try {
    for await (const name of directory.keys()) names.push(name);
  } catch {
    // A directory that will not enumerate leaves nothing to reclaim. Ours is already open.
  }

  for (const name of names) {
    if (keep.includes(name) || !name.startsWith(SPILL_PREFIX)) continue;
    if (name === RESIDENT || name === RESIDENT + INDEX_SUFFIX) continue;
    // Per file, so one live sibling does not stop the sweep reaching the dead ones behind it.
    await directory.removeEntry(name).catch(() => {});
  }
}

async function originPrivateDirectory(): Promise<SpillDirectory> {
  const storage = (navigator as unknown as { storage?: StorageManager }).storage;
  if (!storage?.getDirectory) throw new Error('this browser has no origin private file system');
  return (await storage.getDirectory()) as unknown as SpillDirectory;
}

/** Lets a handle go without letting its failure become the caller's problem. */
function release(handle: SyncAccessHandle): void {
  try {
    handle.close();
  } catch {
    // There is nothing to do about it and nobody to tell: every caller is already unwinding.
  }
}

/** Opens one file and takes its exclusive handle, or throws whatever the browser said. */
async function lock(directory: SpillDirectory, name: string): Promise<SyncAccessHandle> {
  const handle = await directory.getFileHandle(name, { create: true });
  const sync = await handle.createSyncAccessHandle?.();
  if (!sync) throw new Error('this browser has no synchronous access handles');
  return sync;
}

function fileOver(sync: SyncAccessHandle, directory: SpillDirectory, name: string,
                  removeOnClose: boolean): SpillFile {
  return {
    write: (bytes, at) => sync.write(bytes, { at }),
    read: (into, at) => sync.read(into, { at }),
    truncate: (size) => sync.truncate(size),
    size: () => sync.getSize(),
    close: () => {
      // The close and the unlink are two steps and only the second one frees the disk, so a
      // handle that throws on the way out must not take the cleanup with it. Every other call to
      // this handle is already treated as fallible.
      try {
        sync.close();
      } catch {
        // Nothing left to tell. The session is over and the worker is going away with it.
      }
      // Fire and forget: the store closes the sink synchronously, and a tab being torn down has
      // no time to await anything. Whatever this misses, the next session's sweep collects.
      if (removeOnClose) void directory.removeEntry(name).catch(() => {});
    },
  };
}

/**
 * Opens this session's spill tier, or reports why it could not.
 *
 * The resident pair is preferred, because that is the one a reload can come back to: the frames
 * a capture spilled are named by identities its session document carries, and the store hands
 * those identities back on resume (ADR 0029). A tier under a fresh name every run would put those
 * bytes in a file nobody would ever ask for.
 *
 * The handle underneath is exclusive, though, so the resident pair cannot always be had: a second
 * tab open on the app, or a reload whose previous worker has not been torn down yet, gets
 * `NoModificationAllowedError`. Falling back to a name of its own is what keeps that session
 * capturing — with a tier that is not resumable, which is correct, because the capture it would
 * resume belongs to whoever is holding the resident one.
 */
export async function openSpillTier(directory?: SpillDirectory): Promise<SpillTier> {
  const root = directory ?? (await originPrivateDirectory());

  let name = RESIDENT;
  let frames: SyncAccessHandle;
  try {
    frames = await lock(root, RESIDENT);
  } catch {
    // Held by somebody. A tier of its own beats no tier at all.
    name = SPILL_PREFIX + crypto.randomUUID();
    frames = await lock(root, name);
  }

  let index: SyncAccessHandle;
  try {
    index = await lock(root, name + INDEX_SUFFIX);
  } catch (cause) {
    // Half a tier is worse than none: the frame handle would stay locked for the life of the
    // worker, pushing the next session onto a fallback name over a file nobody is using. Best
    // effort, because this is already the recovery path — a throw here would abandon it and cost
    // the session the tier the fallback below exists to give it.
    release(frames);
    if (name !== RESIDENT) throw cause;

    // Two files and two locks, and only one of them has to be unavailable. Giving up here would
    // cost this session its spill tier entirely — a sphere capped at RAM — over a file that holds
    // no pixels. A tier of its own beats no tier at all, the same answer as for the frames.
    name = SPILL_PREFIX + crypto.randomUUID();
    frames = await lock(root, name);
    try {
      index = await lock(root, name + INDEX_SUFFIX);
    } catch (second) {
      release(frames);
      throw second;
    }
  }

  // Swept only once ours are locked, so the sweep cannot reach the files this call is about to
  // take. Another session inside its own window between create and lock can still lose its file
  // to this; it falls to the no-spill path the caller already handles, and closing that window
  // would need a lock protocol of its own for a race two tabs must boot milliseconds apart to
  // reach.
  await reclaimAbandoned(root, [name, name + INDEX_SUFFIX]);

  // The resident pair is left behind when it closes: it is the capture, and deleting it on the
  // way out is exactly what would make a reload find nothing. A fallback pair is nobody's to
  // resume, so it goes with the tab that made it.
  const disposable = name !== RESIDENT;
  return {
    frames: fileOver(frames, root, name, disposable),
    index: fileOver(index, root, name + INDEX_SUFFIX, disposable),
  };
}

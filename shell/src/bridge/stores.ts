/**
 * The two stores the worker opens before the module, in the order a reload needs (ADR 0063).
 *
 * Apart from `worker.ts` because that file is a composition root with side effects on import, and
 * the order is the one thing in it a test has to be able to hold still.
 */
import { createDocumentHost, type DocumentHost } from '../access/document-host';
import { createIndexedDbStore } from '../access/indexeddb-store';
import { createSpillHost, openSpillTier, type SpillHost } from '../access/spill-host';

export interface Stores {
  spill: SpillHost | null;
  documents: DocumentHost;
}

export interface StoreOpeners {
  spill(): Promise<SpillHost>;
  documents(): Promise<DocumentHost>;
}

const BROWSER: StoreOpeners = {
  spill: async () => {
    const tier = await openSpillTier();
    return createSpillHost(tier.frames, tier.index);
  },
  documents: () => createDocumentHost(createIndexedDbStore()),
};

export async function openStores(open: StoreOpeners = BROWSER): Promise<Stores> {
  // The tier can fail on its own and the failure is not fatal: a browser with no origin private
  // file system, or one whose handle will not open, gets a core whose frame store has nowhere to
  // spill. The composition root reads whether this is installed and hands the store a sink or not
  // (ADR 0020), so a sphere on such a browser is capped at what fits in RAM rather than told that
  // spilling freed memory.
  let spill: SpillHost | null;
  try {
    spill = await open.spill();
  } catch (cause) {
    spill = null;
    console.warn('sphanorama worker: no spill tier —', String(cause));
  }

  // After the tier, because on a reload the resident pair is released only when the previous
  // worker is gone, so the snapshot cannot race a write it still had in flight. Not a rescue for
  // the page's `pagehide` flush, which Chromium drops on a reload however this is ordered
  // (ADR 0063). Hydrated whether or not the tier opened, since a missing tier costs spilling and a
  // missing document store costs every session.
  const documents = await open.documents();
  return { spill, documents };
}

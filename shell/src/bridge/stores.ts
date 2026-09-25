/**
 * The two stores the worker opens before the module.
 *
 * Apart from `worker.ts` because that file is a composition root with side effects on import, and
 * which wait the spill tier gets is the one decision in it a test has to be able to reach.
 */
import { createDocumentHost, type DocumentHost } from '../access/document-host';
import { createIndexedDbStore } from '../access/indexeddb-store';
import {
  createSpillHost, handoffFor, openSpillTier, type ResidentAccess, type ResidentHandoff, type SpillHost,
} from '../access/spill-host';

export interface Stores {
  spill: SpillHost | null;
  /** Whether the tier is the resident pair: a page whose worker did not get it gives up the right. */
  resident: boolean;
  documents: DocumentHost;
}

export interface StoreOpeners {
  spill(handoff: ResidentHandoff): Promise<{ host: SpillHost; resident: boolean }>;
  documents(): Promise<DocumentHost>;
}

const BROWSER: StoreOpeners = {
  spill: async (handoff) => {
    const tier = await openSpillTier(undefined, handoff);
    return { host: createSpillHost(tier.frames, tier.index), resident: tier.resident };
  },
  documents: () => createDocumentHost(createIndexedDbStore()),
};

/**
 * `access` is what the page decided this worker may do with the resident pair, because the right to
 * it is a Web Lock the page holds (ADR 0063).
 */
export async function openStores(access: ResidentAccess, open: StoreOpeners = BROWSER): Promise<Stores> {
  // The tier can fail on its own and the failure is not fatal: a browser with no origin private
  // file system, or one whose handle will not open, gets a core whose frame store has nowhere to
  // spill. The composition root reads whether this is installed and hands the store a sink or not
  // (ADR 0020), so a sphere on such a browser is capped at what fits in RAM rather than told that
  // spilling freed memory.
  let spill: SpillHost | null = null;
  let resident = false;
  try {
    ({ host: spill, resident } = await open.spill(handoffFor(access)));
  } catch (cause) {
    console.warn('sphanorama worker: no spill tier —', String(cause));
  }

  // Which of the two opens first does not matter, measured: a debounced write the old worker had
  // not issued dies with it either way, and one it had issued is ordered by IndexedDB itself
  // (ADR 0063). Hydrated whether or not the tier opened, since a missing tier costs spilling and a
  // missing document store costs every session.
  const documents = await open.documents();
  return { spill, resident, documents };
}

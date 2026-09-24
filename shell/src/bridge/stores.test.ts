import { describe, expect, it, vi } from 'vitest';
import type { DocumentHost } from '../access/document-host';
import { handoffFor, type ResidentHandoff, type SpillHost } from '../access/spill-host';
import { openStores } from './stores';

const spillHost = { kind: 'spill' } as unknown as SpillHost;
const documentHost = { kind: 'documents' } as unknown as DocumentHost;

describe('the stores a worker opens before the core', () => {
  it('gives the tier the access the page decided', async () => {
    // The page holds the right to the resident pair, so the page says how this worker may use it
    // (ADR 0063).
    const asked: ResidentHandoff[] = [];
    const open = {
      spill: async (handoff: ResidentHandoff) => { asked.push(handoff); return { host: spillHost, resident: true }; },
      documents: async () => documentHost,
    };

    await openStores('wait', open);
    await openStores('try', open);
    await openStores('skip', open);

    expect(asked).toEqual([handoffFor('wait'), handoffFor('try'), handoffFor('skip')]);
    expect(handoffFor('wait').attempts).toBeGreaterThan(1);
    expect(handoffFor('try').attempts).toBe(1);
    expect(handoffFor('skip').attempts).toBe(0);
  });

  it('opens both, and says whether the tier is the resident one', async () => {
    for (const resident of [true, false]) {
      const stores = await openStores('try', {
        spill: async () => ({ host: spillHost, resident }),
        documents: async () => documentHost,
      });
      expect(stores).toEqual({ spill: spillHost, resident, documents: documentHost });
    }
  });

  it('still opens the documents when there is no tier', async () => {
    const warn = vi.spyOn(console, 'warn').mockImplementation(() => {});
    try {
      const stores = await openStores('try', {
        spill: async () => { throw new Error('no origin private file system'); },
        documents: async () => documentHost,
      });
      expect(stores).toEqual({ spill: null, resident: false, documents: documentHost });
      expect(warn).toHaveBeenCalledWith('sphanorama worker: no spill tier —', expect.stringContaining('no origin private'));
    } finally {
      warn.mockRestore();
    }
  });
});

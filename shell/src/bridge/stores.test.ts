import { describe, expect, it, vi } from 'vitest';
import type { DocumentHost } from '../access/document-host';
import { handoffFor, type ResidentHandoff, type SpillHost } from '../access/spill-host';
import { openStores } from './stores';

const spillHost = { kind: 'spill' } as unknown as SpillHost;
const documentHost = { kind: 'documents' } as unknown as DocumentHost;

describe('the stores a worker opens before the core', () => {
  it('waits for the resident pair on a reload, and only then', async () => {
    // Anything else that waited would be first in line when a reload of the session holding the
    // pair let go, and would take it from that reload (ADR 0063).
    const asked: ResidentHandoff[] = [];
    const open = {
      spill: async (handoff: ResidentHandoff) => { asked.push(handoff); return spillHost; },
      documents: async () => documentHost,
    };

    await openStores(true, open);
    await openStores(false, open);

    expect(asked).toEqual([handoffFor(true), handoffFor(false)]);
    expect(handoffFor(true).attempts).toBeGreaterThan(1);
    expect(handoffFor(false).attempts).toBe(1);
  });

  it('opens both', async () => {
    const stores = await openStores(false, {
      spill: async () => spillHost,
      documents: async () => documentHost,
    });
    expect(stores.spill).toBe(spillHost);
    expect(stores.documents).toBe(documentHost);
  });

  it('still opens the documents when there is no tier', async () => {
    const warn = vi.spyOn(console, 'warn').mockImplementation(() => {});
    try {
      const stores = await openStores(false, {
        spill: async () => { throw new Error('no origin private file system'); },
        documents: async () => documentHost,
      });
      expect(stores.spill).toBeNull();
      expect(stores.documents).toBe(documentHost);
      expect(warn).toHaveBeenCalledWith('sphanorama worker: no spill tier —', expect.stringContaining('no origin private'));
    } finally {
      warn.mockRestore();
    }
  });
});

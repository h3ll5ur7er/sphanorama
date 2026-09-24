import { describe, expect, it, vi } from 'vitest';
import type { DocumentHost } from '../access/document-host';
import type { SpillHost } from '../access/spill-host';
import { openStores } from './stores';

const spillHost = {} as SpillHost;
const documentHost = {} as DocumentHost;

describe('the stores a worker opens before the core', () => {
  it('reads no document until the spill tier is settled', async () => {
    // On a reload the tier is the barrier: the previous worker lets go of it only once it is gone,
    // and until then it may still be committing a debounced write (ADR 0063).
    let letGo!: (host: SpillHost) => void;
    const documents = vi.fn(async () => documentHost);
    const opening = openStores({
      spill: () => new Promise<SpillHost>((resolve) => { letGo = resolve; }),
      documents,
    });

    await Promise.resolve();
    await Promise.resolve();
    expect(documents).not.toHaveBeenCalled();

    letGo(spillHost);
    await expect(opening).resolves.toEqual({ spill: spillHost, documents: documentHost });
    expect(documents).toHaveBeenCalledOnce();
  });

  it('still opens the documents when there is no tier', async () => {
    const warn = vi.spyOn(console, 'warn').mockImplementation(() => {});
    try {
      const stores = await openStores({
        spill: async () => { throw new Error('no origin private file system'); },
        documents: async () => documentHost,
      });
      expect(stores).toEqual({ spill: null, documents: documentHost });
      expect(warn).toHaveBeenCalledWith('sphanorama worker: no spill tier —', expect.stringContaining('no origin private'));
    } finally {
      warn.mockRestore();
    }
  });
});

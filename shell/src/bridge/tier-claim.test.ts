import { describe, expect, it } from 'vitest';
import { tabClaim } from './tier-claim';

function memoryStorage() {
  const items = new Map<string, string>();
  return {
    getItem: (key: string) => items.get(key) ?? null,
    setItem: (key: string, value: string) => { items.set(key, value); },
    removeItem: (key: string) => { items.delete(key); },
  };
}

describe("a tab's claim on the resident spill tier", () => {
  it('is not held in a tab that has never recorded one', () => {
    expect(tabClaim(memoryStorage()).held).toBe(false);
  });

  it('is held by the next page in a tab that got the resident tier', () => {
    const storage = memoryStorage();
    tabClaim(storage).record(true);
    expect(tabClaim(storage).held).toBe(true);
  });

  it('is given up by a page that did not get it, so the one after does not wait', () => {
    // A tab that fell back because a sibling held the pair must not wait on its next reload: it
    // would be first in line when the sibling's own reload let go (ADR 0063).
    const storage = memoryStorage();
    tabClaim(storage).record(true);
    tabClaim(storage).record(false);
    expect(tabClaim(storage).held).toBe(false);
  });

  it('is simply not held where storage is missing or refuses', () => {
    const refusing = {
      getItem: () => { throw new Error('denied'); },
      setItem: () => { throw new Error('denied'); },
      removeItem: () => { throw new Error('denied'); },
    };
    expect(tabClaim(undefined).held).toBe(false);
    expect(tabClaim(refusing).held).toBe(false);
    expect(() => tabClaim(refusing).record(true)).not.toThrow();
    expect(() => tabClaim(undefined).record(true)).not.toThrow();
  });
});

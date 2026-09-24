/**
 * Whether this tab held the resident spill pair last time (ADR 0063).
 *
 * Tab-scoped because the question is: is the worker holding the pair the one this tab is leaving?
 * `sessionStorage` answers it across everything that replaces a page in place — a reload, the same
 * URL again, Back — and says nothing in a new tab, which is exactly the opener that must not wait.
 * A navigation type cannot: `navigate` and `back_forward` also leave a worker behind in this tab,
 * and a reloaded second tab is a `reload` whose sibling holds the pair.
 */
export interface TierClaim {
  held: boolean;
  record(resident: boolean): void;
}

const KEY = 'sphanorama-resident-tier';

type ClaimStorage = Pick<Storage, 'getItem' | 'setItem' | 'removeItem'>;

// Every access guarded: storage can be switched off, and a tab that cannot remember its claim
// only loses the wait, which is what every tab had before this existed. The storage arrives as an
// accessor because switching it off makes Chromium throw from `sessionStorage` itself, before any
// method is reached — read as an argument, that throw escapes to the caller and stops the core
// loading.
export function tabClaim(storageOf: () => ClaimStorage | undefined): TierClaim {
  let storage: ClaimStorage | undefined;
  let held = false;
  try {
    storage = storageOf();
    held = storage?.getItem(KEY) === 'held';
  } catch {
    // No storage, no claim.
  }
  return {
    held,
    record(resident) {
      try {
        if (resident) storage?.setItem(KEY, 'held');
        else storage?.removeItem(KEY);
      } catch {
        // Nothing to do: the next page in this tab falls back at once, as it would have anyway.
      }
    },
  };
}

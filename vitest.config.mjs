import { defineConfig } from 'vitest/config';

export default defineConfig({
  test: {
    // `tools/` too, because `check_dist_fresh.mjs` was the one checker in this repo with no test of
    // its own — every Python checker beside it has a `test_*.py` that `gate.sh` runs, and a
    // reviewer pointed out that a new hard check written test-last, in the one checker nothing
    // checks, is the shape that file exists to complain about.
    include: ['shell/src/**/*.test.ts', 'tools/**/*.test.mjs'],
    environment: 'happy-dom',
  },
});

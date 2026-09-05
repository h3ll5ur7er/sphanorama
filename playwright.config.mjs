import { fileURLToPath } from 'node:url';
import path from 'node:path';

import { defineConfig } from '@playwright/test';

const here = path.dirname(fileURLToPath(import.meta.url));

// The tests start their own servers: the interesting variable is whether the response carries
// COOP/COEP, and Playwright's webServer only describes one configuration.
export default defineConfig({
  testDir: '.',
  testMatch: ['bridge/test/*.spec.mjs', 'shell/e2e/*.spec.mjs'],
  // Agent worktrees are checked out under `.claude/`, and a worktree is a whole second copy of
  // this repository — node_modules and this config included. Playwright refuses to run at all when
  // it finds itself loaded twice, so that copy has to be out of its way.
  //
  // Anchored to *this* file's directory rather than written as `.claude/**`, because Playwright
  // matches these globs against absolute paths: the bare pattern also matches every test inside a
  // worktree, whose own path contains `.claude/`. That reads as "No tests found" from inside one
  // — a whole suite skipped, loudly enough to fail the gate but only after wasting the run.
  //
  // Anchoring is what fixes it, not absence: `.claude/` is tracked in this repository — the
  // engineering skill lives there — so a worktree has one of its own. What it does not have is a
  // `worktrees/` inside it, and more to the point, a worktree's tests sit at `<worktree>/shell/e2e`
  // rather than under `<worktree>/.claude`, so the anchored pattern does not reach them.
  //
  // Nothing in CI has a worktree, since CI clones fresh, which is why this belongs in the file
  // rather than in somebody's memory.
  testIgnore: ['node_modules/**', 'build/**', path.join(here, '.claude', '**')],
  fullyParallel: false,
  reporter: process.env.CI ? 'list' : 'line',
  use: {
    headless: true,
    launchOptions: {
      // A camera that always exists, so the enable path is exercised rather than skipped.
      args: ['--use-fake-device-for-media-stream', '--use-fake-ui-for-media-stream'],
    },
  },
});

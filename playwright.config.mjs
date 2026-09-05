import { defineConfig } from '@playwright/test';

// The tests start their own servers: the interesting variable is whether the response carries
// COOP/COEP, and Playwright's webServer only describes one configuration.
export default defineConfig({
  testDir: '.',
  testMatch: ['bridge/test/*.spec.mjs', 'shell/e2e/*.spec.mjs'],
  // `.claude/**` because agent worktrees are checked out under it, and a worktree is a whole
  // second copy of this repository — node_modules and this config included. Playwright refuses to
  // run at all when it finds itself loaded twice. Nothing in CI has one, since CI clones fresh,
  // which is exactly why this belongs in the file rather than in somebody's memory.
  testIgnore: ['node_modules/**', 'build/**', '.claude/**'],
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

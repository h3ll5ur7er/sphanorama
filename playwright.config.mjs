import { defineConfig } from '@playwright/test';

// The tests start their own servers: the interesting variable is whether the response carries
// COOP/COEP, and Playwright's webServer only describes one configuration.
export default defineConfig({
  testDir: '.',
  testMatch: ['bridge/test/*.spec.mjs', 'shell/e2e/*.spec.mjs'],
  testIgnore: ['node_modules/**', 'build/**'],
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

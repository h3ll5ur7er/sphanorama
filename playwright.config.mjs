import { defineConfig } from '@playwright/test';

// The browser tests start their own servers, because the interesting variable is whether the
// response carries COOP/COEP and Playwright's webServer only describes one configuration.
export default defineConfig({
  testDir: './bridge/test',
  testMatch: '**/*.spec.mjs',
  fullyParallel: false,
  reporter: process.env.CI ? 'list' : 'line',
  use: { headless: true },
});

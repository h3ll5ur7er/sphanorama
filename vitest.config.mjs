import { defineConfig } from 'vitest/config';

export default defineConfig({
  test: {
    include: ['shell/src/**/*.test.ts'],
    environment: 'happy-dom',
  },
});

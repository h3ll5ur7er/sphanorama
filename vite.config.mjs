import { defineConfig } from 'vite';

// base is set from the environment so the same config serves a local dev server at / and GitHub
// Pages at /<repo>/. Hard-coding the repo path would break `npm run dev` for everyone.
export default defineConfig({
  root: 'shell',
  base: process.env.SPHANORAMA_BASE ?? '/',
  build: {
    outDir: '../dist',
    emptyOutDir: true,
    target: 'es2022',
  },
});

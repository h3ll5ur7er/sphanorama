import { defineConfig } from 'vite';
import { execFileSync } from 'node:child_process';

/**
 * Which commit this bundle came from, baked in at build time.
 *
 * A screenshot from a phone is the only evidence some of this project has, and a reading is worth
 * nothing if nobody can tell which commit produced it — a device report and a fix for it crossed
 * in flight once already. `-dirty` is part of the answer rather than a detail: a bundle built over
 * uncommitted edits is not any commit, and saying the hash alone would be a lie about a build
 * nobody else can reproduce.
 *
 * Falls back to a placeholder rather than failing the build: an archive export or a shallow
 * checkout has no git to ask, and refusing to build there would trade a diagnostic for an outage.
 */
function buildId() {
  const git = (...args) => execFileSync('git', args, { encoding: 'utf8' }).trim();
  try {
    return git('rev-parse', '--short', 'HEAD') + (git('status', '--porcelain') ? '-dirty' : '');
  } catch {
    return 'unknown';
  }
}

// base is set from the environment so the same config serves a local dev server at / and GitHub
// Pages at /<repo>/. Hard-coding the repo path would break `npm run dev` for everyone.
export default defineConfig({
  root: 'shell',
  base: process.env.SPHANORAMA_BASE ?? '/',
  define: { __SPHANORAMA_BUILD__: JSON.stringify(buildId()) },
  build: {
    outDir: '../dist',
    emptyOutDir: true,
    target: 'es2022',
  },
});

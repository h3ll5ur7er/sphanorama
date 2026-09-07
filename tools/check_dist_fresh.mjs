/**
 * Refuses to run the browser suite against a bundle older than the sources it was built from.
 *
 * The suite serves `dist/` itself rather than through Playwright's `webServer`, so nothing in the
 * run knows whether that directory was built from the code under test. `npm run build` typechecks
 * first and leaves the previous `dist` in place when it fails — so a run can be green against a
 * bundle from an hour ago, and the failure it hides is the one the change was making. A reviewer
 * hit exactly that twice in one round: two false-green sabotage runs, one of them after
 * `npm run build` had exited 2 on a typecheck error.
 *
 * The gate reads `npm run build`'s exit status and catches it there. This catches it for anyone
 * running `npx playwright test` directly, which is what a person debugging one test actually does.
 *
 * Modification times rather than hashes, because the question is only "was this built after the
 * code changed" and a fresh checkout has no `dist` at all — the missing case is the loud one.
 */
import { readdirSync, statSync, existsSync } from 'node:fs';
import { join, dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const repoRoot = resolve(dirname(fileURLToPath(import.meta.url)), '..');

function newest(path, skip = new Set(['node_modules', '.git', 'dist', 'build'])) {
  let latest = 0;
  let latestPath = path;
  const walk = (at) => {
    let entries;
    try {
      entries = readdirSync(at, { withFileTypes: true });
    } catch {
      return;   // vanished mid-walk, or unreadable; not this check's business
    }
    for (const entry of entries) {
      if (skip.has(entry.name)) continue;
      const full = join(at, entry.name);
      if (entry.isDirectory()) { walk(full); continue; }
      const mtime = statSync(full).mtimeMs;
      if (mtime > latest) { latest = mtime; latestPath = full; }
    }
  };
  if (!existsSync(path)) return { mtime: 0, path };
  if (statSync(path).isDirectory()) walk(path); else latest = statSync(path).mtimeMs;
  return { mtime: latest, path: latestPath };
}

export default function checkDistIsFresh() {
  const dist = join(repoRoot, 'dist');
  if (!existsSync(dist)) {
    throw new Error(
      'dist/ does not exist — the browser suite serves it, so there is nothing to test.\n' +
      'Run `npm run build` in shell/ first (and read its exit status).');
  }

  const built = newest(dist);
  // What the bundle is built from. The wasm is staged by `npm run build` rather than compiled by
  // it, so `shell/public/core` is in the list: a core rebuilt and not staged is the other half of
  // this trap, and the one the skill's own lens text calls out.
  const sources = ['shell/src', 'shell/index.html', 'contracts/ts', 'shell/public']
    .map((rel) => ({ rel, ...newest(join(repoRoot, rel)) }))
    .filter((s) => s.mtime > 0);

  const stale = sources.filter((s) => s.mtime > built.mtime);
  if (stale.length > 0) {
    const worst = stale.reduce((a, b) => (a.mtime > b.mtime ? a : b));
    throw new Error(
      `dist/ is older than the sources it is built from — the browser suite would test a stale\n` +
      `bundle and pass. Newest source: ${worst.path}\n` +
      `Newest built file: ${built.path}\n` +
      'Run `npm run build` in shell/ (and read its exit status: it typechecks first and leaves\n' +
      'the previous dist in place when that fails).');
  }
}

// A static file server with a switch for cross-origin isolation.
//
// The switch is the point. GitHub Pages cannot send COOP/COEP, so "does the core work without
// cross-origin isolation" is a question about the actual deployment target rather than a corner
// case — and the only way to answer it is to serve the artifact both ways and load it.
//
// Usage as a CLI:  node tools/static_server.mjs --root build/wasm-release/bridge [--coi] [--port 8080]
import { createServer } from 'node:http';
import { readFile, stat } from 'node:fs/promises';
import { extname, join, normalize, resolve } from 'node:path';

const CONTENT_TYPES = {
  '.html': 'text/html; charset=utf-8',
  '.js': 'text/javascript; charset=utf-8',
  '.mjs': 'text/javascript; charset=utf-8',
  '.wasm': 'application/wasm',
  '.json': 'application/json',
  '.css': 'text/css; charset=utf-8',
};

/**
 * @param {{roots: string[], crossOriginIsolated?: boolean, port?: number}} options
 * Roots are searched in order, so a test fixture can sit alongside a build directory without
 * either being copied into the other.
 */
export async function startServer({ roots, crossOriginIsolated = false, port = 0 }) {
  const resolved = roots.map((root) => resolve(root));

  const server = createServer(async (request, response) => {
    const urlPath = decodeURIComponent(new URL(request.url, 'http://localhost').pathname);
    // normalize() collapses ".." before it is joined to a root, so a request cannot climb out.
    const relative = normalize(urlPath).replace(/^(\.\.[/\\])+/, '').replace(/^\//, '');

    for (const root of resolved) {
      const candidate = join(root, relative);
      if (!candidate.startsWith(root)) continue;
      try {
        const info = await stat(candidate);
        if (!info.isFile()) continue;
        const headers = { 'Content-Type': CONTENT_TYPES[extname(candidate)] ?? 'application/octet-stream' };
        if (crossOriginIsolated) {
          headers['Cross-Origin-Opener-Policy'] = 'same-origin';
          headers['Cross-Origin-Embedder-Policy'] = 'require-corp';
          headers['Cross-Origin-Resource-Policy'] = 'same-origin';
        }
        response.writeHead(200, headers);
        response.end(await readFile(candidate));
        return;
      } catch {
        // try the next root
      }
    }
    response.writeHead(404, { 'Content-Type': 'text/plain' });
    response.end(`not found: ${relative}`);
  });

  await new Promise((done) => server.listen(port, '127.0.0.1', done));
  const actualPort = server.address().port;
  return {
    origin: `http://127.0.0.1:${actualPort}`,
    async close() {
      await new Promise((done) => server.close(done));
    },
  };
}

if (import.meta.url === `file://${process.argv[1]}`) {
  const args = process.argv.slice(2);
  const roots = args.reduce((acc, arg, i) => (arg === '--root' ? [...acc, args[i + 1]] : acc), []);
  const portIndex = args.indexOf('--port');
  const server = await startServer({
    roots: roots.length ? roots : ['.'],
    crossOriginIsolated: args.includes('--coi'),
    port: portIndex >= 0 ? Number(args[portIndex + 1]) : 8080,
  });
  console.log(`serving ${roots.join(', ')} at ${server.origin}` +
              (args.includes('--coi') ? ' (cross-origin isolated)' : ''));
}

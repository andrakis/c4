#!/usr/bin/env node
// serve.mjs - serve the repo root for the c4m.js page.
//
//   node libjs/serve.mjs [PORT] [--isolate]
//
// The page is libjs/web/index.html. It has to be served from the repo
// ROOT, because the Worker imports ../src/c4bb/sim/... and the page
// fetches images from src/c4bb/images. python3 -m http.server from the
// root works just as well; this exists for --isolate, which adds the two
// headers that make a page cross-origin isolated (and SharedArrayBuffer
// available), for the zero-copy path libjs may grow later.

import { createServer } from 'node:http';
import { readFile, stat } from 'node:fs/promises';
import { extname, join, normalize } from 'node:path';
import { networkInterfaces } from 'node:os';

const ROOT = new URL('..', import.meta.url).pathname;
const args = process.argv.slice(2);
const PORT = Number(args.find(a => /^\d+$/.test(a)) || process.env.LIBJS_PORT || 8472);
const ISOLATE = args.includes('--isolate');

const TYPES = {
  '.html': 'text/html; charset=utf-8', '.js': 'text/javascript', '.mjs': 'text/javascript',
  '.css': 'text/css', '.json': 'application/json', '.txt': 'text/plain; charset=utf-8',
  '.c': 'text/plain; charset=utf-8', '.h': 'text/plain; charset=utf-8',
  '.uc': 'text/plain', '.hwd': 'text/plain', '.svg': 'image/svg+xml', '.png': 'image/png',
};

export function makeServer(root = ROOT, isolate = ISOLATE) {
  return createServer(async (req, res) => {
    let p = join(root, normalize(decodeURIComponent(req.url.split('?')[0])));
    if (!p.startsWith(root)) { res.writeHead(403); res.end(); return; }
    try {
      if ((await stat(p)).isDirectory()) p = join(p, 'index.html');
      const body = await readFile(p);
      const h = { 'content-type': TYPES[extname(p)] || 'application/octet-stream', 'cache-control': 'no-store' };
      if (isolate) {
        h['cross-origin-opener-policy'] = 'same-origin';
        h['cross-origin-embedder-policy'] = 'require-corp';
      }
      res.writeHead(200, h);
      res.end(body);
    } catch { res.writeHead(404); res.end(); }
  });
}

if (import.meta.url === `file://${process.argv[1]}`) {
  makeServer().listen(PORT, '0.0.0.0', () => {
    const lan = Object.values(networkInterfaces()).flat()
      .find(n => n && n.family === 'IPv4' && !n.internal)?.address;
    console.log(`c4m.js: http://localhost:${PORT}/libjs/web/`);
    if (lan) console.log(`        http://${lan}:${PORT}/libjs/web/`);
    console.log(`        https://${PORT}.code.home.stargazer.onl/libjs/web/`);
    if (ISOLATE) console.log('        (cross-origin isolated)');
  });
}

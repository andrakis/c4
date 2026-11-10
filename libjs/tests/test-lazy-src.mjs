// test-lazy-src.mjs - the games' sources are lazy files: C4IX lists them
// in /usr/games, no boot reads one, the first use fetches it, and after
// that it comes from the page's cache (libjs/lazy.js; docs/c4ix-desktop.md,
// part twenty-one). The cache is store.js's memory backend here, as the page's
// is its IndexedDB one. Skips where no game is on the disk.
import { readFileSync } from 'node:fs';
import { join } from 'node:path';
import zlib from 'node:zlib';
import { createMachine, boot } from '../boot.js';
import { typeBytes } from '../keys.js';
import { LazyFiles, cacheMedium, loadCache, keepInCache } from '../lazy.js';
import { memoryBackend } from '../../src/c4bb/web/store.js';

const ROOT = new URL('../../src/c4bb/images/', import.meta.url).pathname;
const DISK = join(ROOT, 'disk');
const index = JSON.parse(readFileSync(join(DISK, 'index.json'), 'utf8'));
const image = new Uint8Array(readFileSync(join(ROOT, 'c4ix32.c4r')));
const srcs = index.filter(e => e[2]);
if (!srcs.length) { console.log('test-lazy-src: no game sources on the disk, skipped'); process.exit(0); }

let fail = 0;
const check = (ok, what, extra = '') => { console.log(`test-lazy-src: ${what} ${ok ? 'OK' : 'FAILED'}${!ok && extra ? '\n' + extra : ''}`); if (!ok) fail = 1; };

// a header of some size, of the first game listed
const [name, size, hash] = srcs.filter(e => /\.h$/.test(e[0]) && e[1] > 1000).sort((a, b) => a[1] - b[1])[0] || srcs[0];
const path = `/usr/${name}`, dir = path.slice(0, path.lastIndexOf('/')), base = name.slice(name.lastIndexOf('/') + 1);
console.log(`test-lazy-src: ${srcs.length} sources on the disk; reading ${path} (${size} bytes)`);

const backend = memoryBackend(), medium = cacheMedium('disk');
const read = n => { try { return new Uint8Array(readFileSync(join(DISK, n))); } catch { return null; } };

// C4IX on a lazy disk with what the cache had, to its shell, its root
// archive unpacked (so all of the boot is over); then each command: its
// output, and the games' files fetched by the time it was done -- not
// their directories' listings (.c4ix-dir), which C4IX reads when it first
// looks in a directory (vfs.c, vfs_lazydir)
const isListing = n => n.endsWith('/.c4ix-dir');
const ofGames = a => a.filter(n => n.startsWith('games/') && !isListing(n));
async function run(cached, cmds) {
  const fetched = [], kept = [];
  const files = new LazyFiles(index, n => { fetched.push(n); return read(n); },
    (n, h, b) => kept.push(keepInCache(backend, medium, n, h, b)));
  files.fill(cached);
  const out = [];
  const vm = createMachine({ arenaMb: 64, drives: [{ files, writable: false, sink: null }], onByte: b => out.push(b) });
  boot(vm, image, ['c4ix32.c4r']);
  const text = (from = 0) => Buffer.from(out.slice(from)).toString('latin1');
  const prompts = from => text(from).split('c4ix:/$').length - 1;
  for (let i = 0; i < 3000 && !prompts(0); i++) vm.run(2e6);
  const task = /unpacking \S+ in the background as task (\d+)/.exec(text());
  const res = { ok: prompts(0) > 0 && !!task, files, outs: [], games: [], listings: [] };
  for (const c of [task ? `wait ${task[1]}` : 'true', ...cmds]) {
    const n0 = out.length;
    typeBytes(vm, new TextEncoder().encode(c + '\n'));
    for (let i = 0; i < 3000 && !prompts(n0); i++) vm.run(2e6);
    res.outs.push(text(n0));
    res.games.push(ofGames(fetched));
    res.listings.push(fetched.filter(isListing));
  }
  res.fetched = fetched;
  await Promise.all(kept);
  return res;
}

// 1. no cache: boot, list the game's directory, read the file twice
// crc32 reads the file whole, and says its CRC and size, which the host's
// copy must have too
const crc = `crc32 ${path}`;
const want = `${zlib.crc32(read(name)).toString(16).padStart(8, '0')} ${size} ${path}`;
const r = await run([], [`ls ${dir}`, crc, crc]);
check(r.ok, 'C4IX boots to its shell, and unpacks its root archive');
check(r.games[0].length === 0, 'the boot fetched no source', r.games[0].join(' '));
const listed = r.listings[1].filter(n => !r.listings[0].includes(n));
check(listed.includes(`${name.slice(0, name.lastIndexOf('/'))}/.c4ix-dir`) && listed.every(isListing) && !r.listings[0].length,
  `ls ${dir} reads its listings, and the boot read none`, `boot: ${r.listings[0].join(' ')}; ls: ${listed.join(' ')}`);
check(r.outs[1].includes(base) && r.games[1].length === 0, `ls ${dir} lists ${base}, and fetches nothing`, r.outs[1]);
check(r.games[2].join() === name, `the first crc32 fetched ${name}, and nothing else of the games'`, r.games[2].join(' '));
check(r.outs[2].includes(want), `crc32 says "${want}", as the host's copy has it`, r.outs[2]);
check(r.outs[3] === r.outs[2] && r.games[3].join() === name, 'the second crc32 fetched nothing', r.outs[3]);
check(r.files.fetched === r.fetched.length && r.files.cached === 0, 'the disk counts the fetches, and nothing from the cache');
const stored = await backend.listFiles(medium);
check(stored.length === 1 && stored[0].name === `${hash} ${name}` && stored[0].data.length === size,
  `the cache keeps it, as "${hash} ${name}"`, stored.map(f => f.name).join(' '));

// 2. a second boot, with what the cache has: the file is not fetched
const cached = await loadCache(backend, medium, index);
check(cached.length === 1 && cached[0][0] === name, 'loadCache gives it back');
const r2 = await run(cached, [crc]);
check(r2.ok && r2.outs[1] === r.outs[2], 'the second boot\'s crc32 says the same', r2.outs[1]);
check(ofGames(r2.fetched).length === 0 && r2.files.cached === 1, 'it came from the cache: nothing of the games\' fetched',
  ofGames(r2.fetched).join(' '));

// 3. a build that changed the file: its hash in the index is another, so
// the cache's copy is dropped, and the file is fetched and kept again
const index2 = index.map(e => e[0] === name ? [e[0], e[1], 'f'.repeat(16)] : e);
const cached2 = await loadCache(backend, medium, index2);
check(cached2.length === 0 && (await backend.listFiles(medium)).length === 0, 'a file whose hash changed is taken out of the cache');
const kept2 = [];
const files2 = new LazyFiles(index2, read, (n, h, b) => kept2.push(keepInCache(backend, medium, n, h, b)));
files2.fill(cached);                       // the old copy: refused, its hash is not the index's
const b2 = files2.get(name);
await Promise.all(kept2);
check(b2 && b2.length === size && files2.fetched === 1 && files2.cached === 0, 'and it is fetched again');
check((await backend.listFiles(medium)).map(f => f.name).join() === `${'f'.repeat(16)} ${name}`, 'and kept under its new hash');
process.exit(fail);

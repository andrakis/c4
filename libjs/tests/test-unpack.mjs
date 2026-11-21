// test-unpack.mjs - a file a disk carries as the package it came in, made
// when the machine opens it (unpack.js): on a disk that is a Map (cli.js)
// and on a lazy one (lazy.js, the page's), by its name in any case, once;
// refused when it is not what its recipe says. Then, where id's package is
// here (games/quake/data/quake106.zip, or $QUAKE_ZIP), Quake's pak0.pak
// made from it by its recipe (games/quake/c4/pak0-unpack.json), and how long
// that takes. Run from the repo root.
import { readFileSync, existsSync } from 'node:fs';
import { zipSync, unzipSync } from '../vendor/fflate.js';
import { fitUnpack } from '../unpack.js';
import { unlh5 } from '../lh5.js';
import { sha256 } from '../sha256.js';
import { LazyFiles } from '../lazy.js';
import { createHash } from 'node:crypto';

let fail = 0;
const ok = (c, what) => { if (c) console.log(`test-unpack: ${what}: OK`); else { console.log(`test-unpack: ${what}: FAILED`); fail = 1; } };
const enc = s => new TextEncoder().encode(s);
const same = (a, b) => !!a && !!b && a.length === b.length && a.every((v, i) => v === b[i]);
const nodeSha = b => createHash('sha256').update(b).digest('hex');

// sha256.js against node's, at the lengths around a block's end
{
  let good = true;
  for (const n of [0, 1, 55, 56, 63, 64, 65, 119, 120, 1000, 100003]) {
    const b = new Uint8Array(n).map((_, i) => (i * 131 + 7) & 255);
    if (sha256(b) !== nodeSha(b)) { good = false; console.log(`  ${n} bytes: ${sha256(b)} not ${nodeSha(b)}`); }
  }
  ok(good, 'sha256.js is SHA-256');
}

// a package, a zip with the file deflated in it, and the recipe beside it
const file = new Uint8Array(300000).map((_, i) => (i % 251) ^ (i >> 9));
const pkg = zipSync({ 'readme.txt': enc('a licence'), 'data/the.bin': [file, { level: 6 }] });
const recipe = sum => enc(JSON.stringify({ from: 'pkg.zip', member: 'data/the.bin', size: file.length, sha256: sum }));
const errors = [];
const quiet = f => { const e = console.error; console.error = m => errors.push(String(m)); try { return f(); } finally { console.error = e; } };

// on a Map, as cli.js loads a directory
{
  const files = new Map([['GAMES/X/pkg.zip', pkg], ['GAMES/X/the.bin.unpack', recipe(nodeSha(file))], ['other', enc('x')]]);
  const logs = [];
  fitUnpack(files, { log: m => logs.push(m) });
  ok(files.has('GAMES/X/the.bin') && files.has('games/x/THE.BIN') && !files.has('GAMES/X/nope.bin'), 'a Map: the made file is there, in any case');
  ok([...files.keys()].includes('GAMES/X/the.bin'), 'a Map: and listed');
  ok(same(files.get('GAMES/X/THE.BIN'), file), 'a Map: its bytes, by its name in another case');
  ok(files.get('GAMES/X/the.bin') === files.get('GAMES/X/the.bin') && files.unpacked.length === 1 && logs.length === 1, 'a Map: made once, then kept');
  ok(files.get('other')[0] === 120 && files.get('missing') === undefined, 'a Map: the other files as they were');
}
// a file of the name on the disk (an upload laid over it) is that file
{
  const files = new Map([['pkg.zip', pkg], ['the.bin.unpack', recipe(nodeSha(file))], ['the.bin', enc('mine')]]);
  fitUnpack(files);
  ok(same(files.get('the.bin'), enc('mine')) && !files.unpacked.length, 'a file of the name is not made');
}
// refused: what is made is not the recipe's
{
  const files = new Map([['pkg.zip', pkg], ['the.bin.unpack', recipe('0'.repeat(64))]]);
  fitUnpack(files);
  errors.length = 0;
  const b = quiet(() => files.get('the.bin'));
  ok(b === undefined && !files.has('the.bin') && errors.length === 1 && /SHA-256/.test(errors[0]), 'a SHA-256 that is not the recipe\'s: refused, and said why');
  quiet(() => files.get('the.bin'));
  ok(errors.length === 1, '... once');
}
// on a lazy disk, as the page's: made where it would be fetched, given to
// the page's cache under the index's hash, the package let go again
{
  const disk = new Map([['pkg.zip', pkg], ['the.bin.unpack', recipe(nodeSha(file))]]);
  const index = [['pkg.zip', pkg.length], ['the.bin.unpack', 1], ['the.bin', file.length, nodeSha(file).slice(0, 16)]];
  const fetched = [], kept = [];
  const files = new LazyFiles(index, n => { fetched.push(n); return disk.get(n) || null; }, (n, h, b) => kept.push([n, h, b]));
  fitUnpack(files);
  const b = files.get('the.bin');
  ok(same(b, file), 'a lazy disk: made');
  ok(fetched.join() === 'the.bin.unpack,pkg.zip', `a lazy disk: the recipe and the package fetched, not the file (${fetched.join()})`);
  ok(kept.length === 1 && kept[0][0] === 'the.bin' && kept[0][1] === nodeSha(file).slice(0, 16), 'a lazy disk: kept for the page under its hash');
  ok(!Map.prototype.has.call(files, 'pkg.zip') && files.has('pkg.zip'), 'a lazy disk: the package let go, and still there');
  // the next visit: the page's cache has it, and nothing is made or fetched
  const again = new LazyFiles(index, n => { fetched.push(n); return disk.get(n) || null; });
  again.fill([['the.bin', nodeSha(file).slice(0, 16), file]]);
  fitUnpack(again);
  fetched.length = 0;
  ok(same(again.get('THE.BIN'), file) && !fetched.length && !again.unpacked.length, 'a lazy disk: from the page\'s cache at the next visit');
  // an index without it: made all the same
  const bare = new LazyFiles(index.slice(0, 2), n => disk.get(n) || null);
  fitUnpack(bare);
  ok(same(bare.get('the.bin'), file), 'a lazy disk whose index does not name it: made');
}

// Quake's pak0.pak, from id's package
const zipPath = process.env.QUAKE_ZIP || 'games/quake/data/quake106.zip';
const recipePath = 'games/quake/c4/pak0-unpack.json';
if (!existsSync(zipPath) || !existsSync(recipePath)) {
  console.log(`test-unpack: Quake's pak0.pak: skipped, no ${existsSync(recipePath) ? zipPath : recipePath}`);
} else {
  const zip = new Uint8Array(readFileSync(zipPath));
  const r = JSON.parse(readFileSync(recipePath, 'utf8'));
  const times = [];
  let b = null;
  for (let i = 0; i < 3; i++) {
    const t0 = performance.now();
    const res = unzipSync(zip, { filter: f => f.name === r.member })[r.member];
    const t1 = performance.now();
    b = unlh5(res, r.lh5[0], r.lh5[1], r.size);
    const t2 = performance.now();
    const sum = sha256(b);
    const t3 = performance.now();
    if (sum !== r.sha256) { b = null; break; }
    times.push([t1 - t0, t2 - t1, t3 - t2, t3 - t0].map(Math.round));
  }
  ok(b && nodeSha(b) === r.sha256, `Quake's pak0.pak made from ${zipPath}, its SHA-256 the recipe's`);
  if (times.length) {
    const best = times.reduce((a, t) => (t[3] < a[3] ? t : a));
    console.log(`test-unpack: pak0.pak, ${r.size} bytes: ${best[3]} ms (inflating resource.1 ${best[0]}, lh5 ${best[1]}, SHA-256 ${best[2]}); runs ${times.map(t => t[3]).join(', ')} ms`);
    ok(best[3] < 5000, 'made in under five seconds');
  }
  // and through a disk, as the machine opens it: the timedemo's way, ./id1/pak0.pak by its last part
  const files = new Map([['quake106.zip', zip], ['pak0.pak.unpack', new Uint8Array(readFileSync(recipePath))]]);
  fitUnpack(files);
  const t0 = performance.now();
  const p = files.get('PAK0.PAK');
  ok(p && p.length === r.size && files.unpacked.length === 1, `a disk with quake106.zip opens pak0.pak (${Math.round(performance.now() - t0)} ms)`);
}
process.exit(fail);

// test-c4ix-free.mjs - free says where C4IX's memory is.
//
// /proc/meminfo (vfs.c) and free (src/c4ix/user/free.c) against the
// heap's own books (heap.js): its size, what is in use, and the biggest
// block it could hand out (SWAP_CMD 6, heap.js largest). Then the heap is
// cut into pieces from outside -- a 384 KB block taken and every other
// one given back -- and free says the free memory is in pieces and how
// big the biggest is; given all back, it does not. And a RAM file written
// is counted as one not saved, until persist save makes it the drive's.
//
//   node libjs/tests/test-c4ix-free.mjs
import { readFileSync } from 'node:fs';
import { join } from 'node:path';
import { createMachine, boot } from '../boot.js';
import { typeBytes } from '../keys.js';
import { fileURLToPath } from 'node:url';

const ROOT = fileURLToPath(new URL('../../src/c4bb/images/', import.meta.url));
const DISK = join(ROOT, 'disk');
class LazyDisk extends Map {
  constructor() { super(); this.sizes = new Map(JSON.parse(readFileSync(join(DISK, 'index.json'), 'utf8'))); }
  has(n) { return super.has(n) || this.sizes.has(n); }
  get(n) {
    if (super.has(n)) return super.get(n);
    if (!this.sizes.has(n)) return undefined;
    const b = new Uint8Array(readFileSync(join(DISK, n))); super.set(n, b); return b;
  }
}
let fail = 0;
const check = (ok, what, extra = '') => {
  console.log(`test-c4ix-free: ${what} ${ok ? 'OK' : 'FAILED'}${!ok && extra ? '\n  ' + String(extra).slice(-1200) : ''}`);
  if (!ok) fail = 1;
};
const store = new Map();
const out = [];
const vm = createMachine({ arenaMb: 32, mmu: true,
  drives: [{ files: new LazyDisk(), writable: false, sink: null },
           { files: new Map(), writable: true, sink: (n, b) => store.set(n, b), remove: n => store.delete(n) }],
  onByte: b => out.push(b) });
boot(vm, new Uint8Array(readFileSync(join(ROOT, 'c4ix32.c4r'))), ['c4ix32.c4r']);
const text = (from = 0) => Buffer.from(out.slice(from)).toString('latin1');
const until = (c, b = 4000) => { for (let i = 0; i < b && !c(); i++) vm.run(2e6); return c(); };
let seq = 0;
const sh = cmds => {
  const n0 = out.length, mark = `@@${++seq}@@`;
  typeBytes(vm, new TextEncoder().encode(`${cmds}\necho ${mark}\n`));
  until(() => text(n0).includes(mark + '\n'));
  return text(n0);
};
const info = t => Object.fromEntries([...t.matchAll(/^([a-z]+) (\d+)$/gm)].map(m => [m[1], +m[2]]));
const row = (t, name) => { const m = new RegExp(`^${name}: +(\\d+) K +(\\d+) K +(\\d+) K +(\\d+) K`, 'm').exec(t); return m ? m.slice(1).map(Number) : null; };
const near = (a, b, slack) => Math.abs(a - b) <= slack;
const h = vm.heap, K = n => n >> 10;

check(until(() => text().includes('c4ix:/$')), 'C4IX boots to its shell, the MMU on');
sh('wait 4');

let t = sh('cat /proc/meminfo');
let m = info(t);
check(m.heap === K(h.end - h.base) && near(m.used, K(h.used), 1024) && near(m.largest, K(h.largest()), 256) &&
  near(m.free, m.heap - m.used, 1) && m.swap > 0 && m.frames >= 0,
  `/proc/meminfo has the heap's size, use and biggest block (${m.heap} K, ${m.used} K, ${m.largest} K)`, t);

t = sh('free');
let r = row(t, 'memory');
check(r && r[0] === m.heap && near(r[1], K(h.used), 1024) && near(r[3], K(h.largest()), 256) && /^swap: /m.test(t) && !/in pieces/.test(t),
  'free prints them, with swap, and the free memory in one piece', t);

// the heap in pieces: 384 KB blocks while they come, every other given back
// (free has to start: its stack is 256 KB and a guard page, in one piece).
// Counted from the last taken, which stays: what no block fitted lies beside
// it, and given back it would make a hole of up to twice the size.
const taken = [];
for (let p; (p = h.alloc(384 << 10)); ) taken.push(p);
for (let i = taken.length - 2; i >= 0; i -= 2) h.free(taken[i]);
t = sh('free');
r = row(t, 'memory');
// A hole can join a free piece too small for a block that was there before
// (one the disk's games left at 148 K made one of 532 K), so under two blocks.
check(r && r[2] > 2048 && r[3] < 768 && /^The free memory is in pieces: nothing bigger than +\d+ K can be had at once\.$/m.test(t),
  `in pieces: ${r && r[2]} K free, nothing bigger than ${r && r[3]} K, and free says so`, t);
for (let i = taken.length - 1; i >= 0; i -= 2) h.free(taken[i]);
t = sh('free');
r = row(t, 'memory');
check(r && r[3] > r[2] / 2 && !/in pieces/.test(t), 'given back, one piece again', t);

// a file written is RAM's until it is saved
const before = info(sh('cat /proc/meminfo'));
sh('cat /build/c4ix.c4r > /usr/copy.c4r');
const written = info(sh('cat /proc/meminfo'));
sh('sync');
const saved = info(sh('cat /proc/meminfo'));
check(written.files - before.files >= 400 && written.nfiles === before.nfiles + 1 && saved.files <= before.files + 64,
  `a file written counts as not saved (${written.files - before.files} K more), and saved it does not (${saved.files} K)`,
  JSON.stringify({ before, written, saved }));

const q = sh('cat /proc/meminfo');
check(/^rqlost 0$/m.test(q), 'no task was found off its queue', q);
process.exit(fail);

// test-sswap.mjs -- sswap.js, the parallel engine's swap pages: in shared
// memory, every thread putting pages out, bringing them in and writing a
// byte of one at once.
//
// Four threads, each with its own slots, put pages of their own pattern
// out, write single bytes into pages they hold (and into slots with none,
// which makes one), bring pages in and forget them, through the same
// devices.js swapOp CPU 0 runs, on a register pair of their own; every
// page they hold is checked after each step. A page another thread was
// given over one of theirs would break a pattern. Each puts 150 pages out
// first, so the pool (256 pages a grow) grows while they all run. Then, on
// one thread: the Map's interface.

import { Worker, isMainThread, parentPort, workerData } from 'node:worker_threads';
import { Arena } from '../../src/c4bb/sim/arena.js';
import { Devices } from '../../src/c4bb/sim/devices.js';
import { SharedSwap, sharedSwapOk } from '../sswap.js';

const MB = 1024 * 1024, SLOTS = 4096, THREADS = 4, STEPS = 3000, PAGE = 4096;
const swapOp = Devices.prototype.swapOp;

if (!isMainThread) {
  const { buf, bufs, id } = workerData;
  const arena = new Arena(buf);
  const sw = new SharedSwap(SLOTS, bufs);
  const dev = { arena, swapPages: sw, swapSlots: SLOTS, swapAddr: 0, machine: null };
  const op = (o, slot, a) => { dev.swapAddr = a; return swapOp.call(dev, o | (slot << 4)); };
  const frame = 0x100000 + id * 0x10000;       // this thread's memory: 16 pages
  const u8 = arena.u8;
  let seed = 4242 + id * 7919;
  const rnd = n => { seed = (seed * 1103515245 + 12345) & 0x7fffffff; return seed % n; };
  const mine = s => id * (SLOTS / THREADS) + s;  // this thread's slots
  const held = new Map();                        // slot -> its bytes, as they should be
  let bad = 0;
  const check = slot => { const want = held.get(slot); for (let i = 0; i < PAGE; i += 61) if (sw.rd8(slot, i) !== want[i]) return false; return true; };
  // 150 pages out each to begin with: the pool grows while all four run
  for (let k = 0; k < 150; k++) {
    const f = frame + (k & 15) * PAGE;
    for (let i = 0; i < PAGE; i++) u8[f + i] = (k + id + i * 3) & 255;
    if (op(1, mine(k), f) !== 0) bad++;
    held.set(mine(k), u8.slice(f, f + PAGE));
  }
  for (let s = 0; s < STEPS && !bad; s++) {
    const slot = mine(rnd(SLOTS / THREADS)), r = rnd(10), f = frame + rnd(16) * PAGE;
    if (r < 4 && !held.has(slot)) {
      const tag = rnd(256);
      for (let i = 0; i < PAGE; i++) u8[f + i] = (tag + i * 7) & 255;
      if (op(1, slot, f) !== 0) bad++;
      held.set(slot, u8.slice(f, f + PAGE));
    } else if (r < 7) {
      const off = rnd(PAGE), v = rnd(256);
      if (!held.has(slot)) held.set(slot, new Uint8Array(PAGE));
      sw.wr8(slot, off, v);
      held.get(slot)[off] = v;
    } else if (held.has(slot)) {
      if (op(2, slot, f) !== 0) bad++;
      const want = held.get(slot);
      for (let i = 0; i < PAGE; i++) if (u8[f + i] !== want[i]) { bad++; break; }
      if (sw.has(slot)) bad++;
      held.delete(slot);
    } else {
      // a slot never written comes in as zeros
      u8.fill(9, f, f + PAGE);
      if (op(2, slot, f) !== 0 || u8[f] !== 0 || u8[f + PAGE - 1] !== 0) bad++;
    }
    if (s % 100 === 0) for (const k of held.keys()) if (!check(k)) bad++;
  }
  for (const k of held.keys()) { if (!check(k)) bad++; op(3, k, 0); }
  parentPort.postMessage({ id, bad });
} else {
  let fail = 0;
  const ok = (what, cond, more = '') => {
    console.log(`test-sswap: ${what} ${cond ? 'OK' : 'FAIL'}${cond || !more ? '' : ' -- ' + more}`);
    if (!cond) fail = 1;
  };
  if (!sharedSwapOk()) { console.log('test-sswap: no growable SharedArrayBuffer here: skipped'); process.exit(0); }
  const arena = new Arena(new SharedArrayBuffer(4 * MB));
  const sw = new SharedSwap(SLOTS, null);

  const results = await Promise.all([...Array(THREADS).keys()].map(id => new Promise((res, rej) => {
    const w = new Worker(new URL(import.meta.url), { workerData: { buf: arena.buf, bufs: sw.bufs, id } });
    w.on('message', res); w.on('error', rej);
  })));
  const bad = results.reduce((s, r) => s + r.bad, 0);
  ok(`${THREADS} threads at once, ${STEPS} steps each: every page kept its bytes`, bad === 0, `${bad} broken`);
  ok(`and everything forgotten, none kept (the pool grew to ${sw.pool.byteLength / PAGE} pages)`,
    sw.size === 0 && [...sw.keys()].length === 0 && sw.pool.byteLength > 256 * PAGE, `size ${sw.size}`);

  // one thread: the Map's interface, as devices.js and arena.js use it
  const p = new Uint8Array(PAGE).fill(5);
  ok('set copies the bytes in, and returns the store', sw.set(7, p) === sw && sw.get(7)[100] === 5 && (p[100] = 6, sw.get(7)[100] === 5));
  ok('get is a view: a write through it is the page', (sw.get(7)[9] = 77, sw.rd8(7, 9) === 77));
  ok('a slot never written reads 0, and has no page', sw.rd8(8, 0) === 0 && !sw.has(8) && sw.get(8) === undefined);
  sw.wr8(8, 4095, 3);
  ok('a byte written to it makes one, zeros but that byte', sw.has(8) && sw.rd8(8, 4095) === 3 && sw.rd8(8, 0) === 0 && sw.size === 2);
  ok('keys are the slots with pages, in order', [...sw.keys()].join() === '7,8');
  ok('slots out of range are refused', (sw.set(SLOTS, p), sw.wr8(-1, 0, 1), !sw.has(SLOTS)) && sw.size === 2);
  ok('delete forgets one', sw.delete(7) && !sw.delete(7) && sw.size === 1);
  const pages = sw.pool.byteLength;
  sw.clear();
  ok('clear forgets them all, the pool kept for later', sw.size === 0 && !sw.has(8) && sw.pool.byteLength === pages);
  sw.set(9, p);
  ok('and a page set after it has its own bytes, from the pool it had', sw.rd8(9, 100) === 6 && sw.rd8(9, 0) === 5 && sw.pool.byteLength === pages);

  console.log(fail ? 'test-sswap: FAILURES' : 'test-sswap: OK');
  process.exit(fail);
}

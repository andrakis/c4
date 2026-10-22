// test-sheap.mjs -- sheap.js, the parallel engine's heap: its books in
// shared memory, every thread allocating and freeing at once.
//
// Four threads each allocate blocks of every size, fill each with its
// own pattern, free some at random and reallocate others, and check every
// block they hold after each step. A block another thread was given over
// one of theirs would break a pattern. At the end every block is freed
// and the heap must count none. Then, on one thread: what heap.js does
// with a pointer malloc never gave, with realloc, and with exhaustion.

import { Worker, isMainThread, parentPort, workerData } from 'node:worker_threads';
import { Arena } from '../../src/c4bb/sim/arena.js';
import { SharedHeap } from '../sheap.js';

const MB = 1024 * 1024, BASE = 0x10000, END = 24 * MB, THREADS = 4, STEPS = 4000;

if (!isMainThread) {
  const { buf, books, id } = workerData;
  const arena = new Arena(buf);
  const heap = new SharedHeap(arena, books);
  const u8 = arena.u8;
  let seed = 12345 + id * 7919;
  const rnd = n => { seed = (seed * 1103515245 + 12345) & 0x7fffffff; return seed % n; };
  const live = [];          // [addr, len, tag]
  const fill = (a, n, tag) => { for (let i = 0; i < n; i++) u8[a + i] = (tag + i) & 255; };
  const good = (a, n, tag) => { for (let i = 0; i < n; i++) if (u8[a + i] !== ((tag + i) & 255)) return false; return true; };
  let bad = 0, tags = 1;
  for (let s = 0; s < STEPS && !bad; s++) {
    const r = rnd(10);
    if (r < 5 || !live.length) {
      const n = rnd(3) ? 1 + rnd(4096) : 4097 + rnd(60000);
      const a = heap.alloc(n);
      if (!a) continue;
      const tag = (id * 64 + tags++) & 255;
      fill(a, n, tag);
      live.push([a, n, tag]);
    } else if (r < 8) {
      const [a, n, tag] = live.splice(rnd(live.length), 1)[0];
      if (!good(a, n, tag)) bad++;
      heap.free(a);
    } else {
      const k = rnd(live.length), [a, n, tag] = live[k];
      const m = 1 + rnd(9000);
      const b = heap.realloc(u8, a, m);
      if (!b) continue;
      if (!good(b, Math.min(n, m), tag)) bad++;
      fill(b, m, tag);
      live[k] = [b, m, tag];
    }
    if (s % 200 === 0) for (const [a, n, tag] of live) if (!good(a, n, tag)) bad++;
  }
  for (const [a, n, tag] of live) { if (!good(a, n, tag)) bad++; heap.free(a); }
  parentPort.postMessage({ id, bad });
} else {
  let fail = 0;
  const ok = (what, cond, more = '') => {
    console.log(`test-sheap: ${what} ${cond ? 'OK' : 'FAIL'}${cond || !more ? '' : ' -- ' + more}`);
    if (!cond) fail = 1;
  };
  const arena = new Arena(new SharedArrayBuffer(32 * MB));
  const heap = new SharedHeap(arena, null);
  heap.reset(BASE, END);

  const results = await Promise.all([...Array(THREADS).keys()].map(id => new Promise((res, rej) => {
    const w = new Worker(new URL(import.meta.url), { workerData: { buf: arena.buf, books: heap.books, id } });
    w.on('message', res); w.on('error', rej);
  })));
  const bad = results.reduce((s, r) => s + r.bad, 0);
  ok(`${THREADS} threads at once, ${STEPS} steps each: every block kept its bytes`, bad === 0, `${bad} broken`);
  ok('and everything freed, the heap counts nothing', heap.count === 0 && heap.used === 0, `count ${heap.count} used ${heap.used}`);

  // one thread: heap.js's rules
  heap.reset(BASE, END);
  const a = heap.alloc(100), b = heap.alloc(5000);
  ok('blocks are 16-byte aligned, in the region', !(a & 15) && !(b & 15) && a >= BASE && b + 5000 <= END);
  heap.free(a + 4);
  heap.free(12345);
  ok('a pointer malloc never gave changes nothing', heap.count === 2);
  arena.u8.fill(7, a, a + 100);
  const c = heap.realloc(arena.u8, a, 200);
  ok('realloc past the span moves the block and its bytes', c !== a && arena.u8[c] === 7 && arena.u8[c + 99] === 7);
  ok('realloc within the span stays', heap.realloc(arena.u8, c, 250) === c);
  ok('realloc of an unknown pointer is 0', heap.realloc(arena.u8, 999, 50) === 0);
  heap.free(c);
  const d = heap.alloc(100);
  ok('a freed small block is used again', d === a);
  ok('non-positive sizes give 0', heap.alloc(0) === 0 && heap.alloc(-5) === 0);
  heap.free(b); heap.free(d);
  // a freed block written over: its broken link is dropped, not followed
  const e = heap.alloc(64), f = heap.alloc(64);
  heap.free(e); heap.free(f);
  arena.i32[f >> 2] = 0x123456;             // f heads the list; its link now points nowhere
  const g = heap.alloc(64), h = heap.alloc(64);
  ok('a free list a guest wrote over is dropped, never followed', g === f && h !== 0x123456 && h !== e);
  heap.free(g); heap.free(h);
  // exhaustion, and a large range given back to the bump pointer
  const big = [];
  for (;;) { const p = heap.alloc(1 << 20); if (!p) break; big.push(p); }
  ok(`exhaustion returns 0 (${big.length} MB given)`, big.length >= 20 && heap.alloc(1 << 20) === 0);
  for (const p of big) heap.free(p);
  ok('everything given back, a block that size fits again', heap.alloc(20 << 20) !== 0);

  console.log(fail ? 'test-sheap: FAILURES' : 'test-sheap: OK');
  process.exit(fail);
}

// test-heap.mjs - the host-side allocator's invariants.
//
// Alignment, no two live blocks overlapping, realloc keeping the bytes it
// promises, unknown pointers ignored, exhaustion returning 0, and freed
// memory being reused rather than leaked.

import { Heap } from '../heap.js';

let fail = 0;
const check = (cond, what) => { if (!cond) { fail = 1; console.log(`test-heap: ${what} FAILED`); } };

const base = 0x10000, end = 0x10000 + 1024 * 1024;
const u8 = new Uint8Array(end);
const h = new Heap(base, end);

// alignment and range
const live = new Map();
let seed = 12345;
const rnd = n => { seed = (Math.imul(seed, 1103515245) + 12345) >>> 0; return seed % n; };
for (let i = 0; i < 2000; i++) {
  if (live.size && rnd(3) === 0) {
    const keys = [...live.keys()];
    const p = keys[rnd(keys.length)];
    h.free(p);
    live.delete(p);
  } else {
    const n = 1 + rnd(rnd(4) === 0 ? 20000 : 200);
    const p = h.alloc(n);
    if (!p) continue;
    check((p & 7) === 0, `alignment of ${p}`);
    check(p >= base && p + n <= end, `range of ${p}+${n}`);
    live.set(p, n);
  }
}
// no overlaps among live blocks
const spans = [...live].sort((x, y) => x[0] - y[0]);
for (let i = 1; i < spans.length; i++)
  check(spans[i - 1][0] + spans[i - 1][1] <= spans[i][0], `overlap at ${spans[i][0]}`);
check(h.count === live.size, `live count ${h.count} vs ${live.size}`);

// realloc keeps min(old, new) bytes
const h2 = new Heap(base, end);
const p = h2.alloc(10);
for (let i = 0; i < 10; i++) u8[p + i] = i + 1;
const q = h2.realloc(u8, p, 5000);
check(q !== 0, 'realloc grow');
for (let i = 0; i < 10; i++) check(u8[q + i] === i + 1, `realloc kept byte ${i}`);
check(h2.sizeOf(p) === -1 || p === q, 'old block released after a move');
const r = h2.realloc(u8, q, 3);
for (let i = 0; i < 3; i++) check(u8[r + i] === i + 1, `realloc shrink kept byte ${i}`);
check(h2.realloc(u8, 0, 16) !== 0, 'realloc(0, n) is malloc');
check(h2.realloc(u8, r, 0) === 0 && h2.sizeOf(r) === -1, 'realloc(p, 0) frees');
check(h2.realloc(u8, 0x12345, 8) === 0, 'realloc of an unknown pointer gives 0');

// unknown frees are ignored; exhaustion gives 0; freed memory comes back
const h3 = new Heap(base, base + 64 * 1024);
h3.free(0x99999);
h3.free(0);
check(h3.alloc(0) === 0 && h3.alloc(-5) === 0, 'non-positive sizes give 0');
check(h3.alloc(1 << 20) === 0, 'exhaustion gives 0');
const blocks = [];
for (let i = 0; i < 100; i++) blocks.push(h3.alloc(500));
check(blocks.every(b => b), 'a hundred small blocks fit');
for (const b of blocks) h3.free(b);
for (let i = 0; i < 100; i++) check(h3.alloc(500) !== 0, `reuse ${i}`);
// Two freed neighbours merge: with a live block after them, the 16000
// bytes can only come from the merged range, starting where big1 did.
const h4 = new Heap(base, base + 64 * 1024);
const big1 = h4.alloc(8000), big2 = h4.alloc(8000);
check(h4.alloc(16) !== 0, 'spacer');
h4.free(big1); h4.free(big2);
check(h4.alloc(16000) === big1, 'freed large neighbours coalesce');

console.log(fail ? 'test-heap: FAILED' : 'test-heap: OK');
process.exit(fail);

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
// The top reached, a small block still comes out of a freed large one.
const h5 = new Heap(base, base + 64 * 1024);
const fill = [];
for (let p; (p = h5.alloc(8000)); ) fill.push(p);
while (h5.alloc(16)) ;
h5.free(fill[1]);
const s5 = h5.alloc(100);
check(s5 >= fill[1] && s5 < fill[1] + 8000, 'a small block from a free range once the top is reached');

// Generations: a block belongs to the generation current when it was
// allocated, and releasing that generation frees it and nothing else.
const h6 = new Heap(base, end);
const dos = h6.alloc(100);                 // generation 0, the boot one
h6.gen = 7;
const kid = [h6.alloc(50), h6.alloc(9000), h6.alloc(3)];
h6.gen = 0;
const dosfile = h6.alloc(200);             // DOS's, made while the child ran
h6.gen = 7;
h6.free(kid[2]);                           // the child freed one of its own
const grown = h6.realloc(u8, kid[0], 6000); // and grew one, which moved
h6.gen = 8;
const grand = h6.alloc(40);                // a program the child ran
const dosgrow = h6.realloc(u8, dosfile, 8000); // DOS's, grown under gen 8
h6.gen = 0;
check(h6.genUsed(7) === 6000 + 9000, `generation 7 holds ${h6.genUsed(7)}`);
check(h6.release(8) === 40 && h6.released === 40, 'release(8) frees the grandchild');
check(h6.sizeOf(grand) === -1, 'the grandchild\'s block is gone');
const before = h6.used;
check(h6.release(7) === 15000, `release(7) frees 15000 bytes, not ${h6.released}`);
check(h6.used === before - 15000, 'used drops by what was released');
check(h6.sizeOf(grown) === -1 && h6.sizeOf(kid[1]) === -1, 'the child\'s blocks are gone, moved ones too');
check(h6.sizeOf(dos) === 100 && h6.sizeOf(dosgrow) === 8000, 'generation 0 keeps its blocks, grown or not');
check(h6.release(7) === 0, 'a released generation is empty');
check(h6.release(0) === 0 && h6.sizeOf(dos) === 100, 'generation 0 cannot be released');
check(h6.gens.size === 0 && h6.byGen.size === 0, 'no books left for released generations');
check(h6.count === 2, `two live blocks, not ${h6.count}`);
// and the space is reused
h6.gen = 9;
check(h6.alloc(9000) !== 0, 'released space is reused');
h6.free(dos); h6.free(dosgrow);

// The registers (src/c4bb/sim/devices.js): HEAP_GEN reads back what was
// written, HEAP_RELEASE frees and reads back the bytes freed.
const { Devices, HEAP_GEN, HEAP_RELEASE, HEAP_USED } = await import('../../src/c4bb/sim/devices.js');
const { Arena } = await import('../../src/c4bb/sim/arena.js');
const dev = new Devices(new Arena(1 << 20), {});
const h7 = new Heap(base, base + 256 * 1024);
dev.machine = { heap: h7 };
dev.write32(HEAP_GEN, 3);
check(dev.read32(HEAP_GEN) === 3 && h7.gen === 3, 'HEAP_GEN reads back');
h7.alloc(1000); h7.alloc(24);
dev.write32(HEAP_GEN, 0);
h7.alloc(16);
check(dev.read32(HEAP_USED) === 1040, 'HEAP_USED counts every generation');
dev.write32(HEAP_RELEASE, 3);
check(dev.read32(HEAP_RELEASE) === 1024 && dev.read32(HEAP_USED) === 16, 'HEAP_RELEASE frees generation 3');
dev.machine = { heap: null };
dev.write32(HEAP_GEN, 5);
check(dev.read32(HEAP_GEN) === 0, 'no heap: HEAP_GEN reads 0');

console.log(fail ? 'test-heap: FAILED' : 'test-heap: OK');
process.exit(fail);

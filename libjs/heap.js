// heap.js - the host side of MALC / FREE / RALC.
//
// Native c4m hands out host malloc() blocks and keeps each block's size
// in a side table (c4m.c:1231-1398), never in a header in front of the
// block, because guests free pointers malloc never produced and a
// header would be read out of whatever happens to be there. This keeps
// the same rule: the arena holds only the guest's bytes, and every
// piece of bookkeeping lives here in JS.
//
// Layout of the region [base, end):
//   - blocks of 16..4096 bytes come from per-size-class free stacks;
//   - larger blocks come from a first-fit list of free ranges, sorted
//     by address and coalesced on free;
//   - anything else is carved from a bump pointer;
//   - and once that is at the end, a small block is carved from a free
//     range too.
// Every block is 8-byte aligned. Fresh memory reads as zero because the
// arena is zero-filled; recycled memory is not cleared (glibc does not
// clear it either, and c4m.c:1055 notes programs only rely on the
// never-recycled case).
//
// Generations (HEAP_GEN, HEAP_RELEASE in src/c4bb/sim/devices.js). Each
// block remembers the generation that was current when it was allocated,
// and release(g) frees every live block of generation g at once. C4DOS
// gives each program a generation of its own and releases it when the
// program ends (DEVICE=HEAP.SYS, docs/c4dos-design.md), so what a game
// allocated and never freed goes back with it. Generation 0 is the one
// the machine boots in: it is never tracked, so a guest that never
// writes HEAP_GEN costs nothing here, and it cannot be released.

const SMALL_MAX = 4096;

function classOf(n) {
  // 16, 32, 64, ... 4096
  let c = 16;
  while (c < n) c <<= 1;
  return c;
}

export class Heap {
  constructor(base, end) {
    this.reset(base, end);
  }

  reset(base, end) {
    this.base = (base + 7) & ~7;
    this.end = end & ~7;
    this.brk = this.base;             // bump pointer
    this.sizes = new Map();           // addr -> requested size
    this.spans = new Map();           // addr -> bytes actually reserved
    this.small = new Map();           // class -> [addr, ...]
    this.large = [];                  // [{addr, len}] sorted by addr
    this.used = 0;                    // bytes requested and not freed
    this.peak = 0;
    this.count = 0;                   // live blocks
    this.gen = 0;                     // the generation new blocks get
    this.gens = new Map();            // addr -> generation, for gen != 0 only
    this.byGen = new Map();           // generation -> Set of its live blocks
    this.released = 0;                // bytes the last release() freed
  }

  tag(addr, g) {
    if (!g) return;
    this.gens.set(addr, g);
    let set = this.byGen.get(g);
    if (!set) this.byGen.set(g, set = new Set());
    set.add(addr);
  }

  untag(addr) {
    const g = this.gens.get(addr);
    if (g === undefined) return 0;
    this.gens.delete(addr);
    const set = this.byGen.get(g);
    set.delete(addr);
    if (!set.size) this.byGen.delete(g);
    return g;
  }

  // Free every live block of generation g; returns the bytes freed (and
  // keeps them in this.released). Generation 0 is refused.
  release(g) {
    g = g | 0;
    this.released = 0;
    const set = g ? this.byGen.get(g) : undefined;
    if (!set) return 0;
    const before = this.used;
    for (const p of [...set]) this.free(p);
    this.byGen.delete(g);
    this.released = before - this.used;
    return this.released;
  }

  // The bytes generation g holds now.
  genUsed(g) {
    const set = this.byGen.get(g | 0);
    if (!set) return 0;
    let n = 0;
    for (const p of set) n += this.sizes.get(p);
    return n;
  }

  // A block of n bytes, or 0 when the region is exhausted. n <= 0 gives
  // 0, which is what c4_malloc does with a non-positive request.
  alloc(n) {
    n = n | 0;
    if (n <= 0) return 0;
    let addr = 0, span = 0;
    if (n <= SMALL_MAX) {
      span = classOf(n);
      const stack = this.small.get(span);
      if (stack && stack.length) addr = stack.pop();
    } else {
      span = (n + 7) & ~7;
      addr = this.takeLarge(span);
    }
    if (!addr) {
      if (this.brk + span <= this.end) { addr = this.brk; this.brk += span; }
      // The bump pointer is at the end, but a free range may still have
      // room for a small block. Without this a heap with megabytes free
      // in its ranges refused a hundred bytes once its top was reached:
      // a full b4ke build inside C4IX did, in 64 MB, and the first block
      // b4ke could not have was its next file name.
      else if (span <= SMALL_MAX) addr = this.takeLarge(span);
      if (!addr) return 0;
    }
    this.sizes.set(addr, n);
    this.spans.set(addr, span);
    if (this.gen) this.tag(addr, this.gen);
    this.used += n;
    this.count++;
    if (this.used > this.peak) this.peak = this.used;
    return addr;
  }

  takeLarge(span) {
    const L = this.large;
    for (let i = 0; i < L.length; i++) {
      const r = L[i];
      if (r.len < span) continue;
      const addr = r.addr;
      if (r.len === span) L.splice(i, 1);
      else { r.addr += span; r.len -= span; }
      return addr;
    }
    return 0;
  }

  // Unknown pointers are ignored. Native c4m would pass them to the
  // host free(); this side is the safer of the two.
  free(p) {
    p = p | 0;
    if (!p) return;
    const span = this.spans.get(p);
    if (span === undefined) return;
    this.used -= this.sizes.get(p);
    this.count--;
    this.sizes.delete(p);
    this.spans.delete(p);
    if (this.gens.size) this.untag(p);
    if (span <= SMALL_MAX) {
      let stack = this.small.get(span);
      if (!stack) this.small.set(span, stack = []);
      stack.push(p);
    } else {
      this.giveLarge(p, span);
    }
  }

  giveLarge(addr, len) {
    const L = this.large;
    // Insertion point by address, then merge with the neighbours.
    let lo = 0, hi = L.length;
    while (lo < hi) { const m = (lo + hi) >> 1; if (L[m].addr < addr) lo = m + 1; else hi = m; }
    L.splice(lo, 0, { addr, len });
    if (lo + 1 < L.length && L[lo].addr + L[lo].len === L[lo + 1].addr) {
      L[lo].len += L[lo + 1].len;
      L.splice(lo + 1, 1);
    }
    if (lo > 0 && L[lo - 1].addr + L[lo - 1].len === L[lo].addr) {
      L[lo - 1].len += L[lo].len;
      L.splice(lo, 1);
      lo--;
    }
    // A free range that ends at the bump pointer goes back to it.
    const last = L[L.length - 1];
    if (last && last.addr + last.len === this.brk) {
      this.brk = last.addr;
      L.pop();
    }
  }

  // The size recorded for p, or -1 if p is not a live block.
  sizeOf(p) {
    const n = this.sizes.get(p | 0);
    return n === undefined ? -1 : n;
  }

  // c4_realloc (c4m.c:1381-1398): p == 0 is malloc; n <= 0 frees and
  // returns 0; an unknown p returns 0 and changes nothing; otherwise
  // the first min(old, n) bytes move to the new block.
  realloc(u8, p, n) {
    p = p | 0; n = n | 0;
    if (!p) return this.alloc(n);
    if (n <= 0) { this.free(p); return 0; }
    const old = this.sizes.get(p);
    if (old === undefined) return 0;
    const span = this.spans.get(p);
    // Grow or shrink in place while it still fits the span.
    if (n <= span) {
      this.used += n - old;
      if (this.used > this.peak) this.peak = this.used;
      this.sizes.set(p, n);
      return p;
    }
    // A block that moves stays in the generation it was allocated in:
    // whoever grows it, it is still its owner's.
    const g = this.gens.get(p) || 0;
    const q = this.alloc(n);
    if (!q) return 0;
    if (g !== this.gen) { this.untag(q); this.tag(q, g); }
    u8.copyWithin(q, p, p + Math.min(old, n));
    this.free(p);
    return q;
  }
}

// sheap.js - heap.js's heap with its books in shared memory, for the
// parallel engine (docs/c4ix-smp.md, "a shared-memory heap"). Every CPU's
// thread allocates and frees itself, under a spinlock; before, every CPU
// but 0 asked CPU 0, a trip through its event loop each time, and a
// compile on CPU 1 made 13,000 of them.
//
// The rules are heap.js's: blocks of 16..4096 bytes come from per-class
// free lists, larger ones from first-fit free ranges sorted by address and
// coalesced, the rest from a bump pointer, and a small block from a free
// range once the bump pointer is at the end. Blocks are 16-byte aligned.
//
// And so is heap.js's first rule, that a guest's bytes cannot be taken for
// the heap's: what a block is, and how big, is in books of their own, a
// SharedArrayBuffer apart from the arena, so freeing a pointer malloc never
// produced changes nothing. One thing is in the arena: a free small block's
// first word links it to the next on its class's list. A guest that writes
// to memory it has freed can break that link -- and the link is checked
// against the books before it is followed, so a broken list is dropped
// (leaked), never handed out.
//
// The books, for an arena of A bytes:
//   ctl     the lock, the region, the bump pointer, the counts
//   heads   each small class's list
//   map     a byte for every 16 bytes: what starts there
//   lspan   a word for every 4096 bytes: the span of the large block that
//           starts there (one at most can: a large block is longer)
//   large   the free ranges, [addr, len] pairs sorted by address
// about 7% of A.
//
// heap.js counts requested bytes as used; this counts the bytes reserved,
// which it can know without a table of requests. realloc copies the old
// block's whole span, which is the old request and bytes after it that
// belong to the block anyway.

const SMALL_MAX = 4096, NCLASS = 9;            // 16 << 0 .. 16 << 8
const C = { LOCK: 0, BASE: 1, END: 2, BRK: 3, USED: 4, PEAK: 5, COUNT: 6, NLARGE: 7, LCAP: 8, HEADS: 16, WORDS: 32 };
const M_FREE = 16, M_LARGE = 32;               // map: 1..9 a used small block of that class, +16 free

function classOf(n) {
  let k = 1, c = 16;
  while (c < n) { c <<= 1; k++; }
  return k;
}
const spanOf = k => 16 << (k - 1);

export class SharedHeap {
  // arena: arena.js's, on a SharedArrayBuffer; books: another heap's
  // books (a CPU's thread joining), or none for new ones
  constructor(arena, books) {
    const size = arena.size;
    if (!books) {
      const lcap = Math.max(1024, size >> 10);
      books = new SharedArrayBuffer(C.WORDS * 4 + (size >> 12) * 4 + lcap * 8 + (size >> 4));
      new Int32Array(books, 0, C.WORDS)[C.LCAP] = lcap;
    }
    this.books = books;
    this.i32 = arena.i32;
    this.ctl = new Int32Array(books, 0, C.WORDS);
    const lcap = this.ctl[C.LCAP];
    let at = C.WORDS * 4;
    this.lspan = new Int32Array(books, at, size >> 12); at += (size >> 12) * 4;
    this.large = new Int32Array(books, at, lcap * 2); at += lcap * 8;
    this.map = new Uint8Array(books, at, size >> 4);
  }

  get used() { return Atomics.load(this.ctl, C.USED); }
  get peak() { return Atomics.load(this.ctl, C.PEAK); }
  get count() { return Atomics.load(this.ctl, C.COUNT); }

  lock() {
    const ctl = this.ctl;
    let spin = 0;
    while (Atomics.compareExchange(ctl, C.LOCK, 0, 1) !== 0) {
      if (++spin > 100) { Atomics.wait(ctl, C.LOCK, 1, 1); spin = 0; }
    }
  }
  unlock() { Atomics.store(this.ctl, C.LOCK, 0); Atomics.notify(this.ctl, C.LOCK, 1); }

  reset(base, end) {
    this.lock();
    const ctl = this.ctl;
    base = (base + 15) & ~15;
    end = end & ~15;
    ctl[C.BASE] = base; ctl[C.END] = end; ctl[C.BRK] = base;
    ctl[C.USED] = 0; ctl[C.PEAK] = 0; ctl[C.COUNT] = 0; ctl[C.NLARGE] = 0;
    for (let k = 0; k < NCLASS; k++) ctl[C.HEADS + k] = 0;
    this.map.fill(0);
    this.lspan.fill(0);
    this.unlock();
  }

  alloc(n) {
    n = n | 0;
    if (n <= 0) return 0;
    this.lock();
    try { return this.take(n); } finally { this.unlock(); }
  }

  free(p) {
    p = p | 0;
    if (!p) return;
    this.lock();
    try { this.give(p); } finally { this.unlock(); }
  }

  // c4_realloc, as heap.js: p == 0 is malloc; n <= 0 frees and returns 0;
  // an unknown p returns 0 and changes nothing
  realloc(u8, p, n) {
    p = p | 0; n = n | 0;
    if (!p) return this.alloc(n);
    if (n <= 0) { this.free(p); return 0; }
    this.lock();
    try {
      const span = this.spanAt(p);
      if (span <= 0) return 0;
      if (n <= span) return p;            // it still fits
      const q = this.take(n);
      if (!q) return 0;
      u8.copyWithin(q, p, p + span);
      this.give(p);
      return q;
    } finally { this.unlock(); }
  }

  // ---- inside the lock ----

  // the span of the live block at p, or 0
  spanAt(p) {
    const ctl = this.ctl;
    if ((p & 15) || p < ctl[C.BASE] || p >= ctl[C.END]) return 0;
    const m = this.map[p >> 4];
    if (m >= 1 && m <= NCLASS) return spanOf(m);
    if (m === M_LARGE) return this.lspan[p >> 12];
    return 0;
  }

  take(n) {
    const ctl = this.ctl, map = this.map;
    let addr = 0, span, k = 0;
    if (n <= SMALL_MAX) {
      k = classOf(n);
      span = spanOf(k);
      addr = this.pop(k);
    } else {
      span = (n + 15) & ~15;
      addr = this.takeLarge(span);
    }
    if (!addr) {
      if (ctl[C.BRK] + span <= ctl[C.END]) { addr = ctl[C.BRK]; ctl[C.BRK] = addr + span; }
      else if (span <= SMALL_MAX) addr = this.takeLarge(span);
      if (!addr) return 0;
    }
    if (k) map[addr >> 4] = k;
    else { map[addr >> 4] = M_LARGE; this.lspan[addr >> 12] = span; }
    ctl[C.USED] += span;
    ctl[C.COUNT]++;
    if (ctl[C.USED] > ctl[C.PEAK]) ctl[C.PEAK] = ctl[C.USED];
    return addr;
  }

  give(p) {
    const ctl = this.ctl, span = this.spanAt(p);
    if (span <= 0) return;                // not a live block: ignored, as heap.js does
    const m = this.map[p >> 4];
    ctl[C.USED] -= span;
    ctl[C.COUNT]--;
    if (m === M_LARGE) {
      this.map[p >> 4] = 0;
      this.lspan[p >> 12] = 0;
      this.giveLarge(p, span);
    } else {
      // on its class's list, linked through its first word
      this.i32[p >> 2] = ctl[C.HEADS + m - 1];
      ctl[C.HEADS + m - 1] = p;
      this.map[p >> 4] = m + M_FREE;
    }
  }

  // a block of class k from its list, or 0; a link that does not lead to
  // a free block of the class drops the rest of the list
  pop(k) {
    const ctl = this.ctl, h = C.HEADS + k - 1;
    const p = ctl[h];
    if (!p) return 0;
    if (this.map[p >> 4] !== k + M_FREE) { ctl[h] = 0; return 0; }
    let next = this.i32[p >> 2] | 0;
    if (next && ((next & 15) || next < ctl[C.BASE] || next >= ctl[C.END] || this.map[next >> 4] !== k + M_FREE)) next = 0;
    ctl[h] = next;
    return p;
  }

  takeLarge(span) {
    const ctl = this.ctl, L = this.large, n = ctl[C.NLARGE];
    for (let i = 0; i < n; i++) {
      const a = L[2 * i], len = L[2 * i + 1];
      if (len < span) continue;
      if (len === span) {
        L.copyWithin(2 * i, 2 * i + 2, 2 * n);
        ctl[C.NLARGE] = n - 1;
      } else { L[2 * i] = a + span; L[2 * i + 1] = len - span; }
      return a;
    }
    return 0;
  }

  giveLarge(addr, len) {
    const ctl = this.ctl, L = this.large;
    let n = ctl[C.NLARGE];
    // the insertion point by address, then merge with the neighbours
    let lo = 0, hi = n;
    while (lo < hi) { const m = (lo + hi) >> 1; if (L[2 * m] < addr) lo = m + 1; else hi = m; }
    if (n >= ctl[C.LCAP]) return;         // no room to remember it: leaked
    L.copyWithin(2 * lo + 2, 2 * lo, 2 * n);
    L[2 * lo] = addr; L[2 * lo + 1] = len;
    n++;
    if (lo + 1 < n && L[2 * lo] + L[2 * lo + 1] === L[2 * lo + 2]) {
      L[2 * lo + 1] += L[2 * lo + 3];
      L.copyWithin(2 * lo + 2, 2 * lo + 4, 2 * n);
      n--;
    }
    if (lo > 0 && L[2 * lo - 2] + L[2 * lo - 1] === L[2 * lo]) {
      L[2 * lo - 1] += L[2 * lo + 1];
      L.copyWithin(2 * lo, 2 * lo + 2, 2 * n);
      n--;
    }
    // a free range that ends at the bump pointer goes back to it
    if (n && L[2 * n - 2] + L[2 * n - 1] === ctl[C.BRK]) { ctl[C.BRK] = L[2 * n - 2]; n--; }
    ctl[C.NLARGE] = n;
  }
}

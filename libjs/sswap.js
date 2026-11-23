// sswap.js - the swap's pages (devices.js swapPages) in shared memory, for
// the parallel engine (par.js): every CPU's thread puts a page out, brings
// it in, and reads and writes a byte of one itself, where before every CPU
// but 0 asked CPU 0, which kept them in a Map.
//
// C4IX gives a task its memory before the task touches it (mmu.c: a slot
// never written is a page of zeros), so swap is busy even with memory to
// spare: a boot and one compile on 4 CPUs brought 6,300 pages in, and the
// kernel's stores into pages not yet there, a byte at a time (c4m.js
// kst8), were 4,096 requests a page. As requests, a boot and a compile on
// 4 CPUs took 25 s against 9.5 s on one.
//
// A Map's interface, as far as devices.js, arena.js and the tests use it:
// get (a view of the page, or undefined), set (copies the bytes in), has,
// delete, clear, keys, size; and rd8/wr8, a byte of a slot's page (wr8
// makes the page, zeros, if the slot has none).
//
// The books, in one SharedArrayBuffer:
//   ctl    the lock, the pages the pool has, how many are free, how many used
//   map    a word per slot: its page in the pool, + 1; 0 for none
//   free   the pool's free pages, a stack
// and the pool, a growable SharedArrayBuffer of pages, grown a megabyte at
// a time up to a page for every slot. Structure changes under a spinlock,
// as sheap.js's; a byte is read and written without it. C4IX swaps under
// its own locks (mmu.c mmu_flock, and only with the kernel lock held), so
// nothing changes a page while another CPU uses it.

const PAGE = 4096, GROW = 256;          // pages a grow adds
const C = { LOCK: 0, NPAGES: 1, NFREE: 2, COUNT: 3, WORDS: 8 };

// Whether this host has growable SharedArrayBuffers (Node 20, Chrome 111,
// Safari 16.4, Firefox 128): without them the pages stay CPU 0's
export function sharedSwapOk() {
  return typeof SharedArrayBuffer !== 'undefined' && typeof SharedArrayBuffer.prototype.grow === 'function';
}

export class SharedSwap {
  // slots: devices.js swapSlots; bufs: another thread's (a CPU joining),
  // or none for new ones
  constructor(slots, bufs) {
    if (!bufs) {
      bufs = {
        books: new SharedArrayBuffer((C.WORDS + 2 * slots) * 4),
        pool: new SharedArrayBuffer(0, { maxByteLength: slots * PAGE }),
      };
    }
    this.bufs = bufs;
    this.slots = slots;
    this.ctl = new Int32Array(bufs.books, 0, C.WORDS);
    this.map = new Int32Array(bufs.books, C.WORDS * 4, slots);
    this.free = new Int32Array(bufs.books, (C.WORDS + slots) * 4, slots);
    this.pool = bufs.pool;
    this.u8 = new Uint8Array(bufs.pool);        // tracks the pool's length as it grows
  }

  lock() {
    const ctl = this.ctl;
    let spin = 0;
    while (Atomics.compareExchange(ctl, C.LOCK, 0, 1) !== 0) {
      if (++spin > 100) { Atomics.wait(ctl, C.LOCK, 1, 1); spin = 0; }
    }
  }
  unlock() { Atomics.store(this.ctl, C.LOCK, 0); Atomics.notify(this.ctl, C.LOCK, 1); }

  get size() { return Atomics.load(this.ctl, C.COUNT); }

  // a slot's page in the pool, + 1, or 0
  page(slot) { return slot >= 0 && slot < this.slots ? Atomics.load(this.map, slot) : 0; }
  has(slot) { return this.page(slot) !== 0; }
  get(slot) {
    const i = this.page(slot);
    return i ? new Uint8Array(this.pool, (i - 1) * PAGE, PAGE) : undefined;
  }
  *keys() { for (let s = 0; s < this.slots; s++) if (Atomics.load(this.map, s)) yield s; }

  // under the lock: a free page of the pool, + 1, the pool grown if it has none
  take() {
    const ctl = this.ctl;
    if (!ctl[C.NFREE]) {
      const n = ctl[C.NPAGES], more = Math.min(GROW, this.slots - n);
      if (more <= 0) throw new RangeError('sswap: a page for every slot is in use');
      this.pool.grow((n + more) * PAGE);
      for (let k = more - 1; k >= 0; k--) this.free[ctl[C.NFREE]++] = n + k + 1;
      ctl[C.NPAGES] = n + more;
    }
    return this.free[--ctl[C.NFREE]];
  }
  // under the lock: SLOT's page, made (zeros) if it has none
  make(slot) {
    let i = this.map[slot];
    if (!i) {
      i = this.take();
      this.u8.fill(0, (i - 1) * PAGE, i * PAGE);
      Atomics.store(this.map, slot, i);
      this.ctl[C.COUNT]++;
    }
    return i;
  }

  // SLOT's page, a copy of BYTES (a page's worth)
  set(slot, bytes) {
    if (slot < 0 || slot >= this.slots) return this;
    this.lock();
    try {
      const i = this.make(slot);
      this.u8.set(bytes.subarray(0, PAGE), (i - 1) * PAGE);
    } finally { this.unlock(); }
    return this;
  }
  delete(slot) {
    if (slot < 0 || slot >= this.slots) return false;
    this.lock();
    try {
      const i = this.map[slot];
      if (!i) return false;
      Atomics.store(this.map, slot, 0);
      this.free[this.ctl[C.NFREE]++] = i;
      this.ctl[C.COUNT]--;
      return true;
    } finally { this.unlock(); }
  }
  // every slot forgotten; the pool's pages kept, all free
  clear() {
    this.lock();
    try {
      const ctl = this.ctl, n = ctl[C.NPAGES];
      this.map.fill(0);
      for (let k = 0; k < n; k++) this.free[k] = n - k;
      ctl[C.NFREE] = n;
      ctl[C.COUNT] = 0;
    } finally { this.unlock(); }
  }

  // a byte of SLOT's page: devices.js swapRd8, swapWr8
  rd8(slot, off) {
    const i = this.page(slot);
    return i ? this.u8[(i - 1) * PAGE + (off & 4095)] : 0;
  }
  wr8(slot, off, v) {
    let i = this.page(slot);
    if (!i) {
      if (slot < 0 || slot >= this.slots) return;
      this.lock();
      try { i = this.make(slot); } finally { this.unlock(); }
    }
    this.u8[(i - 1) * PAGE + (off & 4095)] = v;
  }
}

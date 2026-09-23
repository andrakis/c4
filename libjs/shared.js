// shared.js - the display's SharedArrayBuffer path.
//
// Used only when the page is cross-origin isolated (serve.mjs --isolate,
// on a secure origin); otherwise the Worker posts command batches as
// messages, which is the fallback and the default everywhere else.
//
// Two structures, both made by the page and handed to the Worker:
//
// THE COMMAND RING. The Worker writes the frames gui-device.js drained
// ([len, type, ...payload]) and the page reads them on each animation
// frame. One producer, one consumer, whole frames only. Header words:
//   0 head   words written, a free-running counter (the Worker's)
//   1 tail   words read, a free-running counter (the page's)
//   2 cap    data words, a power of two, so a counter that wraps past
//            2^31 still lands on the right slot (2^32 is a multiple of it)
//   4...     data
// A batch that does not fit is not dropped: the Worker keeps it and
// tries again after the next slice. Order is never lost.
//
// THE FRAMEBUFFER TRIPLE BUFFER. A flip copies the guest's pixels into
// the back slot and publishes it with one atomic exchange; the page swaps
// the newest published slot to the front when it draws. Neither side
// waits, a frame the page never got round to is simply replaced, and the
// ring carries only a marker (CMD.FBREF) that says "draw the framebuffer
// here", so text drawn after a flip still lands on top of it. Header:
//   0 exchange word: the published slot (bits 0-1) | FRESH (bit 2)
//   1 pixels per slot
//   2 frames published
//   4+2i, 5+2i  width and height of slot i
//   16 words in, the three slots

export const RING_CAP = 1 << 22;          // 16 MB of command words
export const FB_MAX = 1 << 20;            // pixels per slot: 1024x1024, 4 MB
const HEAD = 0, TAIL = 1, CAP = 2, DATA = 4;
const FRESH = 4, FB_HDR = 16;

export function makeShared() {
  const ring = new SharedArrayBuffer((DATA + RING_CAP) * 4);
  new Int32Array(ring)[CAP] = RING_CAP;
  const fb = new SharedArrayBuffer((FB_HDR + 3 * FB_MAX) * 4);
  const ctl = new Int32Array(fb, 0, FB_HDR);
  ctl[0] = 1;                             // slot 1 published (not fresh); the page holds 0, the Worker 2
  ctl[1] = FB_MAX;
  return { ring, fb };
}

export class RingWriter {
  constructor(sab) {
    this.w = new Int32Array(sab);
    this.cap = this.w[CAP];
    this.mask = this.cap - 1;
  }
  // Write as many whole frames from words as fit; returns how many words
  // that was, so the caller can keep the rest.
  write(words) {
    const w = this.w;
    const head = w[HEAD];
    const room = this.cap - ((head - Atomics.load(w, TAIL)) | 0);
    let n = 0;
    while (n < words.length) {
      const len = words[n];
      if (len < 2 || n + len > words.length || n + len > room) break;
      n += len;
    }
    if (!n) return 0;
    const at = head & this.mask;
    const first = Math.min(n, this.cap - at);
    w.set(words.subarray(0, first), DATA + at);
    if (first < n) w.set(words.subarray(first, n), DATA);
    Atomics.store(w, HEAD, (head + n) | 0);
    return n;
  }
}

export class RingReader {
  constructor(sab) {
    this.w = new Int32Array(sab);
    this.cap = this.w[CAP];
    this.mask = this.cap - 1;
    this.scratch = new Int32Array(1 << 16);
  }
  // Everything written since the last read, or null. The view is valid
  // until the next call.
  read() {
    const w = this.w;
    const head = Atomics.load(w, HEAD), tail = w[TAIL];
    const n = (head - tail) | 0;
    if (n <= 0) return null;
    if (this.scratch.length < n) this.scratch = new Int32Array(1 << Math.ceil(Math.log2(n)));
    const at = tail & this.mask;
    const first = Math.min(n, this.cap - at);
    this.scratch.set(w.subarray(DATA + at, DATA + at + first), 0);
    if (first < n) this.scratch.set(w.subarray(DATA, DATA + n - first), first);
    Atomics.store(w, TAIL, head);
    return this.scratch.subarray(0, n);
  }
}

export class FbProducer {
  constructor(sab) {
    this.ctl = new Int32Array(sab, 0, FB_HDR);
    this.max = this.ctl[1];
    this.slots = [0, 1, 2].map(i => new Int32Array(sab, (FB_HDR + i * this.max) * 4, this.max));
    this.back = 2;
  }
  // Copy w x h pixels out of guest memory (i32, from byte address base,
  // pitch bytes a row) and publish them. False if they do not fit.
  publish(i32, base, pitch, w, h) {
    if (w * h > this.max) return false;
    const s = this.slots[this.back];
    for (let y = 0; y < h; y++) {
      const row = (base + y * pitch) >> 2;
      s.set(i32.subarray(row, row + w), y * w);
    }
    this.ctl[4 + this.back * 2] = w;
    this.ctl[5 + this.back * 2] = h;
    this.back = Atomics.exchange(this.ctl, 0, this.back | FRESH) & 3;
    Atomics.add(this.ctl, 2, 1);
    return true;
  }
}

export class FbConsumer {
  constructor(sab) {
    this.ctl = new Int32Array(sab, 0, FB_HDR);
    this.max = this.ctl[1];
    this.slots = [0, 1, 2].map(i => new Int32Array(sab, (FB_HDR + i * this.max) * 4, this.max));
    this.front = 0;
    this.gen = 0;                          // bumps whenever a new frame comes to the front
  }
  // The newest published frame: { pixels, w, h, gen }. gen tells the
  // caller whether it has already converted these pixels.
  acquire() {
    if (Atomics.load(this.ctl, 0) & FRESH) {
      this.front = Atomics.exchange(this.ctl, 0, this.front) & 3;
      this.gen++;
    }
    const w = this.ctl[4 + this.front * 2], h = this.ctl[5 + this.front * 2];
    return { pixels: this.slots[this.front], w, h, gen: this.gen };
  }
  published() { return Atomics.load(this.ctl, 2); }
}

// gui-device.js - the display, host side.
//
// A guest draws by writing command frames into a ring in its own memory
// and ringing a bell; the host reads events (mouse, keys) back into a
// second ring. Both rings are exactly c4bb's mailbox format
// (src/c4bb/sim/mbox.js, include/c4bb_mbox.h): the guest hands the device
// one region, the first half carries events to the guest and the second
// half carries commands to the host. HostMailbox is used unchanged.
//
// Registers, one word each, in the window bus.js routes here:
//
//   0x400 CAPS     r    bit0 command ring, bit1 events, bit2 framebuffer, bit3 text
//   0x404 W        r/w  width (a write asks the page to resize)
//   0x408 H        r/w  height
//   0x40C RINGLEN  w    region length in bytes; write before RING
//   0x410 RING     w    region base; lays out both rings. 0 detaches.
//   0x414 BELL     w    1 = flush, 2 = present. r: 1 when events are waiting
//   0x418 EVMASK   r/w  1 move, 2 buttons, 4 keys, 8 resize/focus, 16 wheel
//   0x41C IRQ      r/w  1 = raise HARD_IRQ(3) through the cycle handler on an event
//   0x420 MOUSE    r    (x << 16) | y
//   0x424 BUTTONS  r    button bitmask
//   0x428 TICKS    r    host milliseconds since the device was made
//   0x42C DROPPED  r    events lost to a full ring
//   0x430 FB       r/w  framebuffer base: pixels in guest memory, 0x00RRGGBB
//   0x434 FBPITCH  r/w  bytes from one row to the next (0 = width * 4)
//   0x438 FBFLIP   w    (w << 16) | h: copy that many pixels to the display,
//                       scaled to fill it, then present
//   0x43C FBMODE   r/w  0 = row-major (pixel x,y at base + y*pitch + x*4);
//                       1 = column-major (at base + x*pitch + y*4, pitch
//                       defaulting to h * 4), so a program that draws in
//                       columns -- a raycaster -- can fill each one with a
//                       single memcpy. The page transposes with a canvas
//                       transform, which costs nothing.
//
// In the frames the host makes (FB, FBREF) the height word carries the
// mode in bits 16 and up: h | (mode << 16).
//
// The bell is synchronous: writing it drains the command ring into a
// host-side queue on the spot, inside the store instruction. So a guest
// whose ring fills rings the bell and carries on -- it never has to wait
// for the host to come round, which on a single-threaded host it could
// not do. The Worker ships the queue to the page after each slice.
//
// Announced, never probed: guests test __c4_info() & C4I_GUI (0x4000)
// before touching 0x400, because on native c4m those addresses are just
// memory.

import { HostMailbox } from '../src/c4bb/sim/mbox.js';

export const GUI = {
  CAPS: 0x400, W: 0x404, H: 0x408, RINGLEN: 0x40c, RING: 0x410, BELL: 0x414,
  EVMASK: 0x418, IRQ: 0x41c, MOUSE: 0x420, BUTTONS: 0x424, TICKS: 0x428, DROPPED: 0x42c,
  FB: 0x430, FBPITCH: 0x434, FBFLIP: 0x438, FBMODE: 0x43c,
};
export const CMD = {
  CLEAR: 1, RECT: 2, RECTO: 3, LINE: 4, CIRCLE: 5, TEXT: 6, PIXEL: 7, PRESENT: 8,
  SIZE: 9, IMGDEF: 10, IMG: 11, CLIP: 12, NOCLIP: 13,
  FB: 100,              // host-made: [w, h, ...pixels], from FBFLIP
  FBREF: 101,           // host-made, shared path: [w, h]; the pixels are in the triple buffer (shared.js)
};
export const EV = { MOVE: 1, DOWN: 2, UP: 3, KEYDOWN: 4, KEYUP: 5, RESIZE: 6, FOCUS: 7, WHEEL: 8 };
const MASK = { [EV.MOVE]: 1, [EV.DOWN]: 2, [EV.UP]: 2, [EV.KEYDOWN]: 4, [EV.KEYUP]: 4,
               [EV.RESIZE]: 8, [EV.FOCUS]: 8, [EV.WHEEL]: 16 };
const QUEUE_LIMIT = 8 << 20;             // words of undelivered commands before dropping

export class GuiDevice {
  constructor(opts = {}) {
    this.fitted = true;
    this.w = opts.w || 640;
    this.h = opts.h || 480;
    this.machine = null;
    this.arena = null;
    this.ringLen = 0;
    this.ringBase = 0;
    this.mbox = null;
    this.evmask = 2 | 4 | 8;
    this.irqOn = 0;
    this.irqCount = 0;
    this.mouseX = 0; this.mouseY = 0; this.buttons = 0;
    this.dropped = 0;
    this.queue = [];                     // drained command words: [len, type, ...payload]
    this.chunks = [];                    // earlier queues and framebuffer frames, in order
    this.queued = 0;                     // words in chunks
    this.fbBase = 0; this.fbPitch = 0; this.fbMode = 0; this.flips = 0; this.flipMs = 0;
    this.fbShared = opts.fbShared || null;   // shared.js FbProducer, when the page is isolated
    this.sizeReq = null;
    this.t0 = Date.now();
    this.commands = 0; this.presents = 0; this.events = 0; this.lostCommands = 0;
  }

  attach(machine) {
    this.machine = machine;
    this.arena = machine.arena;
  }

  read32(addr) {
    switch (addr) {
      case GUI.CAPS: return 1 | 2 | 4 | 8;
      case GUI.FB: return this.fbBase;
      case GUI.FBPITCH: return this.fbPitch;
      case GUI.FBMODE: return this.fbMode;
      case GUI.W: return this.w;
      case GUI.H: return this.h;
      case GUI.RINGLEN: return this.ringLen;
      case GUI.RING: return this.ringBase;
      case GUI.BELL: return this.mbox && !this.mbox.inboxEmpty() ? 1 : 0;
      case GUI.EVMASK: return this.evmask;
      case GUI.IRQ: return this.irqOn;
      case GUI.MOUSE: return ((this.mouseX & 0xffff) << 16) | (this.mouseY & 0xffff);
      case GUI.BUTTONS: return this.buttons;
      case GUI.TICKS: return (Date.now() - this.t0) | 0;
      case GUI.DROPPED: return this.dropped;
      default: return 0;
    }
  }

  write32(addr, v) {
    switch (addr) {
      case GUI.W: this.w = Math.max(1, Math.min(4096, v | 0)); this.sizeReq = { w: this.w, h: this.h }; return;
      case GUI.H: this.h = Math.max(1, Math.min(4096, v | 0)); this.sizeReq = { w: this.w, h: this.h }; return;
      case GUI.RINGLEN: this.ringLen = v | 0; return;
      case GUI.RING: {
        const base = v | 0;
        if (!base) { this.ringBase = 0; this.mbox = null; return; }
        // Two rings of at least a few frames each, inside the arena, on
        // word boundaries. Anything else detaches rather than letting the
        // host write through a wild pointer.
        const len = this.ringLen & ~7;
        if ((base & 3) || len < 256 || base < 0x1000 || base + len > this.arena.size) {
          this.ringBase = 0; this.mbox = null; return;
        }
        this.ringBase = base;
        this.mbox = new HostMailbox(this.arena, { base, len });
        return;
      }
      case GUI.BELL:
        this.pull();
        return;
      case GUI.EVMASK: this.evmask = v | 0; return;
      case GUI.FB: this.fbBase = v | 0; return;
      case GUI.FBPITCH: this.fbPitch = v | 0; return;
      case GUI.FBMODE: this.fbMode = v & 1; return;
      case GUI.FBFLIP: this.flip((v >>> 16) & 0xffff, v & 0xffff); return;
      case GUI.IRQ: this.irqOn = v ? 1 : 0; return;
      default: return;
    }
  }

  // Take every command the guest has written.
  pull() {
    if (!this.mbox) return;
    let frames;
    try { frames = this.mbox.drain(); }
    catch { this.mbox.layout(); return; }          // a corrupt ring: start it over
    for (const f of frames) {
      if (this.queued + this.queue.length + 2 + f.payload.length > QUEUE_LIMIT) { this.lostCommands++; continue; }
      this.queue.push(2 + f.payload.length, f.type);
      for (let i = 0; i < f.payload.length; i++) this.queue.push(f.payload[i]);
      this.commands++;
      if (f.type === CMD.PRESENT) this.presents++;
    }
  }

  // Everything queued since the last call, as one Int32Array of
  // [len, type, ...payload] frames, or null. The ring is emptied too, so
  // a guest that wrote frames and never rang still gets them drawn.
  drain() {
    this.pull();
    this.seal();
    if (!this.chunks.length) return null;
    const words = new Int32Array(this.queued);
    let at = 0;
    for (const c of this.chunks) { words.set(c, at); at += c.length; }
    this.chunks = []; this.queued = 0;
    return words;
  }

  // Words drain() handed out that the shared ring had no room for: they
  // go back at the front, ahead of anything queued since, so order holds.
  putBack(words) {
    if (!words.length) return;
    const c = words.slice();
    this.chunks.unshift(c); this.queued += c.length;
  }

  // Close off the commands queued so far as one chunk, keeping order with
  // the framebuffer frames that go between them.
  seal() {
    if (!this.queue.length) return;
    const c = Int32Array.from(this.queue);
    this.queue = [];
    this.chunks.push(c); this.queued += c.length;
  }

  // A framebuffer frame: w x h pixels from guest memory, copied now (the
  // guest may start the next frame the moment the store completes), and
  // queued behind every command written before it. The page draws it
  // scaled to the display and presents.
  flip(w, h) {
    const t0 = performance.now();
    try { this.flipInner(w, h); } finally { this.flipMs += performance.now() - t0; }
  }

  flipInner(w, h) {
    this.pull();
    if (!w || !h || !this.fbBase) return;
    // Copied as `lines` runs of `run` words, `pitch` bytes apart: rows for
    // row-major, columns for column-major.
    const mode = this.fbMode;
    const lines = mode ? w : h, run = mode ? h : w;
    const pitch = this.fbPitch || run * 4;
    const base = this.fbBase;
    const hm = h | (mode << 16);
    if ((base & 3) || (pitch & 3) || base < 0x1000 || base + (lines - 1) * pitch + run * 4 > this.arena.size) return;
    const n = w * h;
    // Shared path: one copy into the triple buffer, and a small marker in
    // the command stream where the frame belongs.
    if (this.fbShared && this.fbShared.publish(this.arena.i32, base, pitch, run, lines, w, hm)) {
      this.queue.push(4, CMD.FBREF, w, hm);
      this.flips++;
      return;
    }
    if (this.queued + this.queue.length + n + 4 > QUEUE_LIMIT) { this.lostCommands++; return; }
    this.seal();
    // A framebuffer frame covers the whole display, so one still waiting
    // with nothing after it is simply out of date: replace it.
    const last = this.chunks[this.chunks.length - 1];
    if (last && last[1] === CMD.FB) { this.chunks.pop(); this.queued -= last.length; this.skippedFlips = (this.skippedFlips | 0) + 1; }
    const c = new Int32Array(4 + n);
    c[0] = 4 + n; c[1] = CMD.FB; c[2] = w; c[3] = hm;
    const i32 = this.arena.i32;
    for (let y = 0; y < lines; y++) {
      const row = (base + y * pitch) >> 2;
      c.set(i32.subarray(row, row + run), 4 + y * run);
    }
    this.chunks.push(c); this.queued += c.length;
    this.flips++;
  }

  takeSizeRequest() {
    const r = this.sizeReq;
    this.sizeReq = null;
    return r;
  }

  // An event from the page: { kind, x, y, button, buttons, code, char, mods, w, h, dy, focus }
  event(m) {
    const x = m.x | 0, y = m.y | 0;
    let type, payload;
    switch (m.kind) {
      case 'move':    type = EV.MOVE;    payload = [x, y, m.buttons | 0]; break;
      case 'down':    type = EV.DOWN;    payload = [x, y, m.button | 0]; break;
      case 'up':      type = EV.UP;      payload = [x, y, m.button | 0]; break;
      case 'keydown': type = EV.KEYDOWN; payload = [m.code | 0, m.char | 0, m.mods | 0]; break;
      case 'keyup':   type = EV.KEYUP;   payload = [m.code | 0, m.char | 0, m.mods | 0]; break;
      case 'resize':  type = EV.RESIZE;  payload = [m.w | 0, m.h | 0]; this.w = m.w | 0; this.h = m.h | 0; break;
      case 'focus':   type = EV.FOCUS;   payload = [m.focus ? 1 : 0]; break;
      case 'wheel':   type = EV.WHEEL;   payload = [x, y, m.dy | 0]; break;
      default: return;
    }
    if (type <= EV.UP || type === EV.WHEEL) { this.mouseX = x; this.mouseY = y; }
    if (type === EV.MOVE) this.buttons = m.buttons | 0;
    if (type === EV.DOWN) this.buttons |= 1 << (m.button | 0);
    if (type === EV.UP) this.buttons &= ~(1 << (m.button | 0));
    if (!this.mbox || !(this.evmask & MASK[type])) return;
    this.events++;
    if (!this.mbox.send(type, payload)) { this.dropped++; return; }
    if (this.irqOn) this.irqCount++;
  }

  irqPending() { return this.irqCount > 0; }
  irqTaken() { this.irqCount = 0; }

  stats() {
    return { w: this.w, h: this.h, attached: !!this.mbox, commands: this.commands,
             presents: this.presents, events: this.events, dropped: this.dropped,
             lostCommands: this.lostCommands, flips: this.flips, flipMs: Math.round(this.flipMs) };
  }
}

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
//   0x430 FB, 0x434 FBPITCH, 0x438 FBFLIP   reserved for a pixel framebuffer
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
  FB: 0x430, FBPITCH: 0x434, FBFLIP: 0x438,
};
export const CMD = {
  CLEAR: 1, RECT: 2, RECTO: 3, LINE: 4, CIRCLE: 5, TEXT: 6, PIXEL: 7, PRESENT: 8,
  SIZE: 9, IMGDEF: 10, IMG: 11, CLIP: 12, NOCLIP: 13,
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
      case GUI.CAPS: return 1 | 2 | 8;
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
      if (this.queue.length + 2 + f.payload.length > QUEUE_LIMIT) { this.lostCommands++; continue; }
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
    if (!this.queue.length) return null;
    const words = Int32Array.from(this.queue);
    this.queue = [];
    return words;
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
             lostCommands: this.lostCommands };
  }
}

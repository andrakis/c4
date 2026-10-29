// snd-device.js - the sound card, host side: an AdLib (an OPL2, or the
// OPL3 it grew into), eight channels of 8-bit samples, and the PC
// speaker. The machine never makes a sample. What it writes becomes
// events stamped with when they happen, and whoever listens -- the
// page's AudioWorklet, or cli.js --wav writing a file -- mixes them with
// snd-mix.js.
//
// Registers, one word each, in the window bus.js routes here:
//
//   0x480 CAPS      r    bit0 OPL, bit1 samples, bit2 speaker, bit3 OPL3
//   0x484 OPL       w    (reg << 8) | val, reg 0x000-0x1FF (0x100 up is the OPL3's second bank)
//   0x488 WAIT      w    n: the stream moves n CLOCK units on
//   0x48C AHEAD     r    microseconds the stream is ahead of now
//   0x490 CLOCK     r/w  the stream's units per second (default 1000000)
//   0x494 PCM_ADDR  r/w  samples: unsigned 8-bit, mono, in guest memory
//   0x498 PCM_LEN   r/w  how many
//   0x49C PCM_RATE  r/w  at what rate, Hz
//   0x4A0 PCM_VOL   r/w  left | (right << 8), 0-255 each
//   0x4A4 PCM_PLAY  w    ch 0-7: play ADDR/LEN/RATE/VOL there now (the bytes are copied now)
//   0x4A8 PCM_STOP  w    ch, or -1 for all of them
//   0x4AC PCM_SETVOL w   ch: PCM_VOL becomes its volume, as it plays
//   0x4B0 PCM_BUSY  r    a bit for each channel still playing
//   0x4B4 SPK       w    the PIT's divisor for the speaker's tone (1193180 / Hz); 0 is quiet
//   0x4B8 RESET     w    everything quiet, the OPL's registers zero
//
// The stream. An OPL write is stamped at the stream's cursor, which WAIT
// moves on, so a player that makes music the way a sound card's
// interrupt would -- write some registers, wait a tick, write more --
// says exactly when each write happens, however late it runs. It keeps
// itself ahead by reading AHEAD and making more while that is short;
// a cursor that falls behind now is brought up to it. Everything else
// is stamped now.
//
// Now is the host's: the machine's own clock in cli.js, so a file
// rendered from a session is the same every time, and the page's clock
// in the Worker, because the AudioContext plays in real time whether or
// not the machine keeps up.
//
// Announced, never probed: guests test __c4_info() & C4I_SND (0x8000).

export const SND_BASE = 0x480, SND_END = 0x500;
export const SND = {
  CAPS: 0x480, OPL: 0x484, WAIT: 0x488, AHEAD: 0x48c, CLOCK: 0x490,
  PCM_ADDR: 0x494, PCM_LEN: 0x498, PCM_RATE: 0x49c, PCM_VOL: 0x4a0,
  PCM_PLAY: 0x4a4, PCM_STOP: 0x4a8, PCM_SETVOL: 0x4ac, PCM_BUSY: 0x4b0,
  SPK: 0x4b4, RESET: 0x4b8,
};
// what the listener gets: [kind, t (us), ...], in the order they happened
//   [OPL, t, reg, val]   [PCM, t, ch, rate, vol, bytes]   [STOP, t, ch | -1]
//   [VOL, t, ch, vol]    [SPK, t, divisor]                [RESET, t]
export const EVT = { OPL: 1, PCM: 2, STOP: 3, VOL: 4, SPK: 5, RESET: 6 };

const CHANNELS = 8;
const PCM_MAX = 4 << 20;                 // bytes in one sample
const EVENT_LIMIT = 1 << 16;             // undelivered events before the oldest go

export class SoundDevice {
  // opts.now: () => microseconds; the machine's own clock by default
  constructor(opts = {}) {
    this.fitted = true;
    this.nowFn = opts.now || null;
    this.machine = null;
    this.arena = null;
    this.events = [];
    this.lost = 0;
    this.clock = 1000000;
    this.cursor = 0;
    this.pcm = { addr: 0, len: 0, rate: 8000, vol: 0xffff };
    this.ends = new Float64Array(CHANNELS);   // when each channel's sound is over, us
    this.writes = 0;
    this.plays = 0;
  }

  attach(machine) {
    this.machine = machine;
    this.arena = machine.arena;
  }

  now() {
    if (this.nowFn) return this.nowFn();
    const m = this.machine, d = m.dev;
    return m.cycle * 1000 / d.cyclesPerMs + d.sleepMs * 1000;
  }

  emit(e) {
    if (this.events.length >= EVENT_LIMIT) { this.events.shift(); this.lost++; }
    this.events.push(e);
  }

  // the stream's cursor, never behind now
  at() {
    const n = this.now();
    if (this.cursor < n) this.cursor = n;
    return this.cursor;
  }

  read32(addr) {
    switch (addr) {
      case SND.CAPS: return 1 | 2 | 4 | 8;
      case SND.AHEAD: return Math.max(0, Math.min(0x7fffffff, Math.round(this.cursor - this.now())));
      case SND.CLOCK: return this.clock;
      case SND.PCM_ADDR: return this.pcm.addr;
      case SND.PCM_LEN: return this.pcm.len;
      case SND.PCM_RATE: return this.pcm.rate;
      case SND.PCM_VOL: return this.pcm.vol;
      case SND.PCM_BUSY: {
        const n = this.now();
        let m = 0;
        for (let c = 0; c < CHANNELS; c++) if (this.ends[c] > n) m |= 1 << c;
        return m;
      }
      default: return 0;
    }
  }

  write32(addr, v) {
    switch (addr) {
      case SND.OPL:
        this.emit([EVT.OPL, this.at(), (v >>> 8) & 0x1ff, v & 0xff]);
        this.writes++;
        return;
      case SND.WAIT:
        if (v > 0) this.cursor = this.at() + v * 1e6 / this.clock;
        return;
      case SND.CLOCK: this.clock = Math.max(1, v | 0); return;
      case SND.PCM_ADDR: this.pcm.addr = v >>> 0; return;
      case SND.PCM_LEN: this.pcm.len = v | 0; return;
      case SND.PCM_RATE: this.pcm.rate = Math.max(100, Math.min(200000, v | 0)); return;
      case SND.PCM_VOL: this.pcm.vol = v & 0xffff; return;
      case SND.PCM_PLAY: {
        const c = v & (CHANNELS - 1), { addr, len, rate, vol } = this.pcm;
        // inside the arena, or nothing: never a copy through a wild pointer
        if (len <= 0 || len > PCM_MAX || addr < 0x1000 || addr + len > this.arena.size) return;
        this.arena.dma(addr, len, false);   // the task's own samples, or its page fault
        const t = this.now();
        this.emit([EVT.PCM, t, c, rate, vol, this.arena.u8.slice(addr, addr + len)]);
        this.ends[c] = t + len * 1e6 / rate;
        this.plays++;
        return;
      }
      case SND.PCM_STOP: {
        const t = this.now();
        if ((v | 0) < 0) this.ends.fill(0); else this.ends[v & (CHANNELS - 1)] = 0;
        this.emit([EVT.STOP, t, (v | 0) < 0 ? -1 : v & (CHANNELS - 1)]);
        return;
      }
      case SND.PCM_SETVOL:
        this.emit([EVT.VOL, this.now(), v & (CHANNELS - 1), this.pcm.vol]);
        return;
      case SND.SPK: this.emit([EVT.SPK, this.now(), v & 0xffff]); return;
      case SND.RESET:
        this.ends.fill(0);
        this.emit([EVT.RESET, this.now()]);
        return;
      default: return;
    }
  }

  // Everything since the last drain, oldest first; null if nothing
  drain() {
    if (!this.events.length) return null;
    const e = this.events;
    this.events = [];
    return e;
  }

  stats() { return { writes: this.writes, plays: this.plays, lost: this.lost }; }
}

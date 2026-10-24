// ap.js - one processor other than CPU 0, in a thread of its own: a Web
// Worker in a page, a worker_threads Worker in Node (par.js, which
// starts it; docs/c4ix-smp.md S2).
//
// It runs its Machine on the shared arena, flat out, while its state is
// RUN; parks in Atomics.wait on the guest's word for CWAI; sleeps in
// Atomics.wait for USLP; and waits on its state word while it is off or
// halted, until a CPUS starts it again. What reaches the devices or the
// console it asks CPU 0 for, through its request slot. The heap it uses
// itself: its books are shared (sheap.js).

import { Arena } from '../src/c4bb/sim/arena.js';
import { OP, STOP, Machine } from './c4m.js';
import { C, K, S, R, KIND, ParCore, POSTED, PQLEN, PQSTRIDE } from './par.js';
import { SharedHeap } from './sheap.js';

const word = (k, f) => C.CPU0 + k * C.STRIDE + f;

// the opcodes whose work is CPU 0's: the devices, the console
const ASK = new Set([OP.OPEN, OP.READ, OP.CLOS, OP.PRTF,
  OP.PUTC, OP.PUTS, OP.STRC, OP._OPC, OP.DBG]);
// guarded in protected mode, as on CPU 0 (c4m.js sys): a protected task's
// gets the local trap, never CPU 0's service
const GUARDED = new Set([OP.OPEN, OP.READ, OP.CLOS, OP.PRTF, OP.MALC, OP.FREE, OP.RALC,
  OP.PUTC, OP.PUTS, OP.EXIT, OP.USLP, OP.INFO]);

// Devices this CPU never touches itself; what a Machine looks at
class ApDev {
  constructor(ctl, cyclesPerMs) {
    this.ctl = ctl; this.cyclesPerMs = cyclesPerMs;
    this.halted = false; this.status = 0;
    this.pitMs = 0; this.mboxIrq = 0; this.mbox = null; this.sleepMs = 0;
    this.onByte = null; this.uartActivity = 0;
  }
  simMs() { return Atomics.load(this.ctl, C.TIMEMS) | 0; }
  hostNow() { return Date.now(); }
}

class ApMachine extends Machine {
  constructor(arena, dev, bus, info, core) {
    super(arena, dev, bus, null);
    this.fixedInfo = info;
    this.core = core;
    this.posted = 0;                    // writes posted since CPU 0 was last told
  }
  info() { return this.fixedInfo; }

  // A posted write (par.js POSTED): queued for CPU 0, which applies it
  // before it answers this CPU's next question. A full queue waits for room.
  post(addr, val, w8) {
    const q = this.core.pq, b = this.cpuId * PQSTRIDE, t = q[b + 1];
    for (;;) {
      const h = Atomics.load(q, b);
      if (((t - h) | 0) < PQLEN) break;
      this.core.ring();
      Atomics.wait(q, b, h, 5);
      if (Atomics.load(this.core.ctl, C.HALT)) return;
    }
    const e = b + 2 + 2 * (t & (PQLEN - 1));
    q[e] = addr | (w8 ? 0x10000 : 0);
    q[e + 1] = val;
    Atomics.store(q, b + 1, (t + 1) | 0);
    this.posted = 1;
  }
  // CPU 0 told there are posted writes to apply, if it may be asleep
  flushPosted() { if (this.posted) { this.posted = 0; this.core.ring(); } }

  // one question for CPU 0, and wait for its answer
  ask(kind, op, addr, val) {
    const ctl = this.core.ctl, b = word(this.cpuId, 0);
    ctl[b + K.KIND] = kind; ctl[b + K.OP] = op; ctl[b + K.SP] = this.sp; ctl[b + K.A] = this.a;
    ctl[b + K.RPC] = this.pc;
    ctl[b + K.ADDR] = addr | 0; ctl[b + K.VAL] = val | 0;
    Atomics.store(ctl, b + K.REQ, R.ASK);
    this.core.ring();
    while (Atomics.load(ctl, b + K.REQ) === R.ASK) {
      Atomics.wait(ctl, b + K.REQ, R.ASK, 100);
      if (Atomics.load(ctl, C.HALT)) { this.dev.halted = true; return STOP.HALT; }
    }
    const ra = ctl[b + K.RA], rstop = ctl[b + K.RSTOP];
    Atomics.store(ctl, b + K.REQ, R.IDLE);
    return { ra, rstop };
  }

  sys(op) {
    if (this.mode && GUARDED.has(op)) return this.pmTrap(op);
    if (ASK.has(op)) {
      const r = this.ask(KIND.OP, op, 0, 0);
      if (r === STOP.HALT) return STOP.HALT;
      if (r.rstop === STOP.INPUT) { this.pc -= 4; return STOP.INPUT; }   // try again later
      this.a = r.ra;
      return STOP.BUDGET;
    }
    if (op === OP.TIME) { this.a = this.dev.simMs(); return STOP.BUDGET; }
    if (op === OP.USLP) {
      // this CPU sleeps; the others carry on
      this.flushPosted();
      const ms = Math.max(1, Math.floor(this.ld32(this.sp) / 1000));
      Atomics.wait(this.core.ctl, word(this.cpuId, K.SLEEP), 0, ms);
      this.a = 0;
      return STOP.BUDGET;
    }
    if (op === OP.EXIT) return this.halt(this.ld32(this.sp));
    return super.sys(op);
  }

  // EXIT: this processor, never the machine
  halt(status) {
    this.cpuState = 'halt';
    this.cpuStatus = status | 0;
    Atomics.store(this.core.ctl, word(this.cpuId, K.STATE), S.HALT);
    this.core.ring();
    return STOP.CPUHALT;
  }
}

// Device registers are CPU 0's (bus.js and arena.js's ranges); everything
// else is the shared arena's, as on CPU 0's bus: unaligned words, the low
// memory below 0x1000 (the ROM, the trap word), and the ends of the arena,
// read and written by arena.js, whose io is off here. (Reading unaligned
// words as 0, as this once did, broke every C4R header a CPU other than 0
// read: the loader's words start at byte 13.)
class ApBus {
  constructor(arena) { this.arena = arena; this.m = null; }
  dev(addr) { return (addr >= 0x100 && addr < 0x200) || (addr >= 0x400 && addr < 0x500); }
  read32(addr) {
    addr = addr >>> 0;
    if (this.dev(addr)) { const r = this.m.ask(KIND.RD32, 0, addr & ~3, 0); return r === STOP.HALT ? 0 : r.ra | 0; }
    return this.arena.read32(addr) | 0;
  }
  write32(addr, val) {
    addr = addr >>> 0;
    if (this.dev(addr)) {
      if (POSTED.has(addr & ~3)) { this.m.post(addr & ~3, val | 0, 0); return; }
      this.m.ask(KIND.WR32, 0, addr & ~3, val);
      return;
    }
    this.arena.write32(addr, val | 0);
  }
  read8s(addr) { return this.arena.read8s(addr); }
  write8(addr, val) {
    addr = addr >>> 0;
    if (this.dev(addr)) {
      if (POSTED.has(addr & ~3)) { this.m.post(addr, val & 0xff, 1); return; }
      this.m.ask(KIND.WR8, 0, addr, val & 0xff);
      return;
    }
    this.arena.write8(addr, val);
  }
}

function main(msg) {
  const arena = new Arena(msg.arena);
  const ctl = new Int32Array(msg.ctl);
  const id = msg.id;
  const core = new ParCore(ctl, arena.i32, msg.n, new Int32Array(msg.pq));
  const dev = new ApDev(ctl, msg.cyclesPerMs);
  const bus = new ApBus(arena);
  const m = new ApMachine(arena, dev, bus, msg.info, core);
  arena.io = null;                    // device registers go through ApBus, to CPU 0
  bus.m = m;
  m.noFpu = msg.noFpu;
  m.heap = new SharedHeap(arena, msg.heap);
  m.smp = core;
  m.cpuId = id;
  m.cpuState = 'off';
  m.local = new Int32Array(64);
  m.localU8 = new Uint8Array(m.local.buffer);
  m.ipiPending = 0; m.waitAddr = 0; m.waitVal = 0; m.cpuStatus = 0;
  const st = word(id, K.STATE);

  for (;;) {
    // off or halted: wait to be started (or for the machine to stop)
    while (Atomics.load(ctl, st) !== S.RUN) {
      if (Atomics.load(ctl, C.HALT)) return;
      Atomics.wait(ctl, st, Atomics.load(ctl, st), 1000);
    }
    if (Atomics.load(ctl, C.HALT)) return;
    const entry = Atomics.load(ctl, word(id, K.ENTRY)), stack = Atomics.load(ctl, word(id, K.STACK));
    m.reset();
    m.sp = (stack - 4) | 0;
    m.i32[m.sp >> 2] = 0x18;            // CPU_HALT_ADDR: the entry function's LEV lands on CPUH
    m.bp = m.sp; m.pc = entry | 0; m.a = 0;
    m.ipiPending = 0;
    m.cpuState = 'run';
    Atomics.store(ctl, word(id, K.IPI), 0);

    for (;;) {
      // a slice, then an IPI or the machine's end looked for: not inside
      // the instruction loop, which would run it at half speed
      const r = m.run(65536);
      m.flushPosted();
      core.poll(m);
      Atomics.store(ctl, word(id, K.CYCLES), m.cycle | 0);
      if (dev.halted || Atomics.load(ctl, C.HALT)) return;
      if (r === STOP.CPUHALT) break;
      if (r === STOP.PARK) {
        // wait for the word, a CWAK, an IPI or the machine's end; then
        // CWAI returns 0 and its caller looks again
        Atomics.wait(m.i32, m.waitAddr >> 2, m.waitVal, 1000);
        if (Atomics.load(ctl, C.HALT)) return;
        if (Atomics.load(ctl, word(id, K.IPI))) { Atomics.store(ctl, word(id, K.IPI), 0); m.ipiPending = 1; }
        m.cpuState = 'run';
        Atomics.store(ctl, word(id, K.WAITADDR), 0);
        Atomics.store(ctl, st, S.RUN);
      } else if (r === STOP.INPUT) {
        Atomics.wait(ctl, word(id, K.SLEEP), 0, 5);   // a key may come; ask again soon
      }
    }
  }
}

// a page's Worker, or Node's worker_threads
const isNode = typeof process !== 'undefined' && process.versions && process.versions.node &&
  typeof self === 'undefined';
if (isNode) {
  const { parentPort } = await import('node:worker_threads');
  parentPort.once('message', msg => { main(msg); process.exit(0); });
} else {
  self.onmessage = e => { if (e.data.type === 'init') { main(e.data); self.close(); } };
}

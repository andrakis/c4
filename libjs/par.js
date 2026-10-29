// par.js - several processors for real: one thread per CPU, sharing the
// arena (docs/c4ix-smp.md S2). The parallel engine; smp.js is the
// simulated one, and the two run the same opcodes.
//
// CPU 0 is the thread the hosts already drive (cli.js's, or worker.js's
// Worker); each other CPU is a worker of its own running ap.js. They
// share two SharedArrayBuffers:
//
//   - the ARENA, which every CPU's Machine reads and writes as it always
//     has. CAS, XCHG and FADD are Atomics on the guest's own word; CWAI
//     is Atomics.wait on it and CWAK is Atomics.notify, so a wait that
//     races a wake loses nothing: the wait checks the value itself;
//   - the CONTROL block (Int32Array, layout below): each CPU's state, its
//     pending IPI, the word it waits on, the machine's time, and a
//     request slot per CPU.
//
// The heap is every CPU's too: its books are a third SharedArrayBuffer
// (sheap.js), and MALC, FREE and RALC run on the CPU that executes them.
//
// CPU 0 owns everything else that is not memory: the devices and the
// console. Another CPU that runs an opcode reaching those -- OPEN, READ,
// PRTF and the rest -- or touches a device register
// writes the request into its slot, rings CPU 0's doorbell and waits.
// CPU 0 answers between slices of 16K cycles (ParHost.run) and, when it
// is not running, as soon as the doorbell rings
// (Atomics.waitAsync). Its answer is what the opcode would have done on
// CPU 0, run by a shadow Machine with the asking CPU's registers.
//
// CPU 0 cannot block its thread for long -- the page's messages arrive
// there -- so its CWAI parks the way a blocked read does: run() returns
// STOP.PARK, and the host waits for the word, a CWAK, an IPI or a request
// a few ms at a time, answering requests as they come (parkSync).

import { OP, STOP, Machine } from './c4m.js';
import { DmaSpace, FAULT } from './mmu.js';

// the control block, in words
export const C = {
  HALT: 0,                      // the machine has stopped: every CPU leaves
  DOOR: 1,                      // CPU 0's doorbell: bumped and notified with each request
  TIMEMS: 2,                    // the machine's time in ms, as CPU 0 last published it
  GUARD: 3, GLO: 4, GHI: 5,     // the boot stack's guard (devices.js STACK_GUARD): on, and where
  CPU0: 16, STRIDE: 28,         // CPU k's words start at CPU0 + k * STRIDE
};
export const K = {              // offsets within a CPU's words
  STATE: 0, ENTRY: 1, STACK: 2, IPI: 3, WAITADDR: 4,
  REQ: 5, KIND: 6, OP: 7, SP: 8, A: 9, ADDR: 10, VAL: 11, RA: 12, RSTOP: 13, CYCLES: 14, SLEEP: 15,
  WAITVAL: 16, PC: 17,          // where it parked, for a deadlock's report
  WOKE: 18,                     // CPU 0's: a CWAK for the word it waits on (parkSync)
  RPC: 19,                      // the asker's pc: PRTF's argument count is the ADJ after it
  BP: 20,                       // ... and its bp, for a guard hit's report,
  GN: 21, GFROM: 22, GW: 23,    // ... and the rest of it (c4m.js guardHit's n, from; GW: write | mode << 1)
  PTBR: 24,                     // a device access's: the asker's space when it is in user mode, else 0
  FADDR: 25, FINFO: 26,         // ... and what the device refused there (RSTOP DMA_FAULT)
  UDIR: 27,                     // the asker's UDIR: the space its kernel mode reaches user addresses in
};
// RSTOP for a device access the asker's space refused (mmu.js dmaCheck):
// the asker raises the page fault, with FADDR and FINFO
export const DMA_FAULT = -77;
export const S = { OFF: 0, RUN: 1, WAIT: 2, HALT: 3 };             // a CPU's state
export const R = { IDLE: 0, ASK: 1, DONE: 2 };                     // a request slot's
export const KIND = { OP: 1, RD32: 2, WR32: 3, WR8: 4, GUARD: 5 }; // what is asked
export const MAXCPU = 32;
const word = (k, f) => C.CPU0 + k * C.STRIDE + f;

// Posted writes: the device registers whose writes need no answer and
// read no guest memory -- the sound card's OPL, WAIT, its PCM settings,
// STOP, SETVOL, the speaker and RESET (snd-device.js). Another CPU queues
// them for CPU 0 and carries on, where every other device access asks and
// waits. CPU 0 applies a CPU's queue, in order, before it answers that
// CPU's next request, and between its own slices. A game's music is
// thousands of OPL writes a second, and each one a round trip made a game
// on CPU 1 slower than on CPU 0: Doom's demo3 under C4IX, on two CPUs, with
// the display and the card, took 63.0 s there against 53.5 s (cli-like,
// in Node), with 119k of its 135k requests these writes. PCM_PLAY copies
// the samples, and the display's BELL and FBFLIP read the frame, so they
// still ask.
export const POSTED = new Set([0x484, 0x488, 0x490, 0x494, 0x498, 0x49c, 0x4a0, 0x4a8, 0x4ac, 0x4b4, 0x4b8]);
// Each CPU's queue, in a SharedArrayBuffer of its own: its head (CPU 0's,
// the next to apply), its tail (the CPU's, the next free), then PQLEN
// entries of two words, (8-bit << 16) | addr and the value.
export const PQLEN = 1024;
export const PQSTRIDE = 2 + 2 * PQLEN;

// What the parallel opcodes do, on whichever thread runs them. m is the
// running CPU's Machine; ctl the control block; i32 the arena's.
export class ParCore {
  constructor(ctl, i32, n, pq) { this.ctl = ctl; this.ai32 = i32; this.n = n; this.pq = pq; this.parallel = true; this.running = true; }

  state(k) { return Atomics.load(this.ctl, word(k, K.STATE)); }

  // c4mp's opcodes 66-75 (c4m.js smpOp hands them here)
  op(m, op) {
    const sp = m.sp, ctl = this.ctl, i32 = this.ai32;
    const aligned = t => (t & 3) === 0 && t >= 0x1000 && t + 4 <= i32.length * 4;
    switch (op) {
      case OP.CPUI: m.a = m.cpuId; return STOP.BUDGET;
      case OP.CPUN: m.a = this.n; return STOP.BUDGET;
      case OP.CPUS: m.a = this.start(m.ld32(sp + 8), m.ld32(sp + 4), m.ld32(sp)); return STOP.BUDGET;
      case OP.CPUH:
        m.cpuState = 'halt';
        Atomics.store(ctl, word(m.cpuId, K.STATE), S.HALT);
        this.ring();
        return STOP.CPUHALT;
      case OP.CAS: {
        const t = m.ld32(sp + 8), e = m.ld32(sp + 4), v = m.ld32(sp);
        if (aligned(t)) m.a = Atomics.compareExchange(i32, t >> 2, e, v);
        else { const o = m.ld32(t); if (o === e) m.st32(t, v); m.a = o; }
        return STOP.BUDGET;
      }
      case OP.XCHG: {
        const t = m.ld32(sp + 4), v = m.ld32(sp);
        if (aligned(t)) m.a = Atomics.exchange(i32, t >> 2, v);
        else { const o = m.ld32(t); m.st32(t, v); m.a = o; }
        return STOP.BUDGET;
      }
      case OP.FADD: {
        const t = m.ld32(sp + 4), d = m.ld32(sp);
        if (aligned(t)) m.a = Atomics.add(i32, t >> 2, d);
        else { const o = m.ld32(t); m.st32(t, (o + d) | 0); m.a = o; }
        return STOP.BUDGET;
      }
      case OP.CWAI: {
        const t = m.ld32(sp + 4), val = m.ld32(sp);
        if (!t || !aligned(t) || Atomics.load(i32, t >> 2) !== val) { m.a = 1; return STOP.BUDGET; }
        m.a = 0;
        m.waitAddr = t; m.waitVal = val;
        m.cpuState = 'wait';
        Atomics.store(ctl, word(m.cpuId, K.WOKE), 0);
        Atomics.store(ctl, word(m.cpuId, K.WAITVAL), val);
        Atomics.store(ctl, word(m.cpuId, K.PC), m.pc);
        Atomics.store(ctl, word(m.cpuId, K.CYCLES), m.cycle | 0);
        Atomics.store(ctl, word(m.cpuId, K.WAITADDR), t);
        Atomics.store(ctl, word(m.cpuId, K.STATE), S.WAIT);
        return STOP.PARK;
      }
      case OP.CWAK: {
        const t = m.ld32(sp + 4), n = m.ld32(sp);
        m.a = t && aligned(t) ? Atomics.notify(i32, t >> 2, n > 0 ? n : undefined) : 0;
        // CPU 0 waits on its doorbell, not on the word (parkSync)
        if (t && m.cpuId !== 0 && Atomics.load(ctl, word(0, K.WAITADDR)) === t) {
          Atomics.store(ctl, word(0, K.WOKE), 1);
          this.ring();
          if (!m.a) m.a = 1;
        }
        return STOP.BUDGET;
      }
      case OP.IPI: m.a = this.ipi(m.ld32(sp)); return STOP.BUDGET;
    }
    return STOP.BUDGET;
  }

  // __c4_cpu_start: a halted or never-started CPU takes the function and
  // stack, and its thread, waiting on its state word, runs them
  start(id, entry, stacktop) {
    if (id < 0 || id >= this.n || !entry || !stacktop) return 0;
    const ctl = this.ctl, st = word(id, K.STATE);
    const was = Atomics.load(ctl, st);
    if (was !== S.OFF && was !== S.HALT) return 0;
    Atomics.store(ctl, word(id, K.ENTRY), entry);
    Atomics.store(ctl, word(id, K.STACK), stacktop);
    if (Atomics.compareExchange(ctl, st, was, S.RUN) !== was) return 0;
    Atomics.notify(ctl, st);
    return 1;
  }

  // __c4_ipi: the flag, then whatever the target may be waiting on
  ipi(id) {
    if (id < 0 || id >= this.n) return 0;
    const ctl = this.ctl, s = this.state(id);
    if (s !== S.RUN && s !== S.WAIT) return 0;
    Atomics.store(ctl, word(id, K.IPI), 1);
    const wa = Atomics.load(ctl, word(id, K.WAITADDR));
    if (wa) Atomics.notify(this.ai32, wa >> 2);
    Atomics.notify(ctl, word(id, K.SLEEP));      // a USLP ends early, as on smp.js
    if (id === 0) this.ring();
    return 1;
  }

  // at an instruction boundary: an IPI for m, or the machine stopping
  poll(m) {
    const ctl = this.ctl, w = word(m.cpuId, K.IPI);
    if (Atomics.load(ctl, C.HALT)) { m.dev.halted = true; return true; }
    if (Atomics.load(ctl, w)) { Atomics.store(ctl, w, 0); m.ipiPending = 1; }
    return false;
  }

  ring() { Atomics.add(this.ctl, C.DOOR, 1); Atomics.notify(this.ctl, C.DOOR); }
}

// CPU 0's side: the workers, the answers, the park
export class ParHost extends ParCore {
  // m0: CPU 0, on an arena whose buffer is a SharedArrayBuffer.
  // spawn(url): a worker running ap.js (new Worker in a page, worker_threads in Node)
  constructor(m0, n, spawn) {
    n = Math.max(1, Math.min(MAXCPU, n | 0));
    const ctl = new Int32Array(new SharedArrayBuffer((C.CPU0 + MAXCPU * C.STRIDE) * 4));
    super(ctl, m0.i32, n, new Int32Array(new SharedArrayBuffer(n * PQSTRIDE * 4)));
    this.running = false;           // m0.run comes here first (c4m.js), then slices
    this.m0 = m0;
    this.dev = m0.dev;
    m0.smp = this;
    m0.cpuId = 0; m0.cpuState = 'run';
    m0.local = new Int32Array(64);
    m0.localU8 = new Uint8Array(m0.local.buffer);
    m0.ipiPending = 0; m0.waitAddr = 0; m0.waitVal = 0; m0.cpuStatus = 0;
    Atomics.store(ctl, word(0, K.STATE), S.RUN);
    this.dev.timeSource = this;
    this.idleCycles = 0;            // machine time that passed while CPU 0 was parked
    this.seenDoor = 0;
    this.shadows = [];
    this.workers = [];
    for (let k = 1; k < n; k++) {
      // the asking CPU's registers, on CPU 0's devices and heap
      const s = new Machine(m0.arena, m0.dev, m0.bus, m0.heap);
      s.noFpu = m0.noFpu;
      s.cpuId = k;
      this.shadows[k] = s;
      const w = spawn(new URL('./ap.js', import.meta.url));
      w.postMessage({
        type: 'init', arena: m0.arena.buf, ctl: ctl.buffer, pq: this.pq.buffer, heap: m0.heap.books, id: k, n,
        info: m0.info(), cyclesPerMs: m0.dev.cyclesPerMs, noFpu: !!m0.noFpu,
      });
      this.workers.push(w);
    }
    this.dev.machine = m0;
    // the guard is every CPU's: each looks at the control block
    this.dev.onGuard = () => {
      Atomics.store(ctl, C.GLO, this.dev.stackBase | 0);
      Atomics.store(ctl, C.GHI, this.dev.stackTop | 0);
      Atomics.store(ctl, C.GUARD, this.dev.stackGuard | 0);
    };
    this.dev.onGuard();
  }

  // the machine's time: CPU 0's cycles, and what passed while it waited
  get cycle() { return this.m0.cycle + this.idleCycles; }

  // CPU 0 runs in slices of 16K cycles, and between them answers the
  // others and takes its IPIs: checking for them inside the instruction
  // loop made every CPU run at half speed. A request waits at most a
  // slice -- well under a millisecond.
  run(maxCycles) {
    const m0 = this.m0;
    let left = maxCycles, r = STOP.BUDGET;
    this.running = true;
    try {
      while (left > 0) {
        const before = m0.cycle;
        r = m0.run(Math.min(left, 16384));
        left -= Math.max(1, m0.cycle - before);
        this.poll(m0);
        if (r !== STOP.BUDGET || m0.dev.halted) break;
      }
    } finally { this.running = false; }
    return m0.dev.halted ? STOP.HALT : r;
  }

  // between CPU 0's slices: answer anyone waiting, and apply what was
  // posted, then its own IPI
  poll(m) {
    if (Atomics.load(this.ctl, C.DOOR) !== this.seenDoor) this.service();
    else for (let k = 1; k < this.n; k++) this.applyPosted(k);
    return super.poll(m);
  }

  // CPU k's posted writes, in the order it made them
  applyPosted(k) {
    const q = this.pq, b = k * PQSTRIDE, t = Atomics.load(q, b + 1);
    let h = Atomics.load(q, b);
    if (h === t) return;
    const bus = this.m0.bus;
    while (h !== t) {
      const e = b + 2 + 2 * (h & (PQLEN - 1)), a = q[e], v = q[e + 1];
      if (a & 0x10000) bus.write8(a & 0xffff, v);
      else bus.write32(a, v);
      h = (h + 1) | 0;
    }
    Atomics.store(q, b, h);
    Atomics.notify(q, b);               // a CPU waiting for room
  }

  // Every request waiting, answered; and the time published
  service() {
    const ctl = this.ctl;
    this.seenDoor = Atomics.load(ctl, C.DOOR);
    Atomics.store(ctl, C.TIMEMS, this.dev.simMs());
    for (let k = 1; k < this.n; k++) {
      const b = word(k, 0);
      this.applyPosted(k);              // before its request: they came first
      if (Atomics.load(ctl, b + K.REQ) !== R.ASK) continue;
      const kind = ctl[b + K.KIND];
      let ra = 0, rstop = STOP.BUDGET;
      if (kind === KIND.OP) {
        const s = this.shadows[k];
        s.sp = ctl[b + K.SP]; s.a = ctl[b + K.A]; s.mode = 0; s.pc = ctl[b + K.RPC]; s.cycleInterval = 0;
        // its kernel mode's user window (c4m.js kpa), as on its own CPU
        s.uwin = this.m0.uwin; s.udir = ctl[b + K.UDIR];
        s.symbols = this.m0.symbols;
        this.dev.machine = s;
        rstop = s.sys(ctl[b + K.OP]);
        this.dev.machine = this.m0;
        ra = s.a;
      } else if (kind === KIND.GUARD) {
        // its guard hit, told and halted here, where the console is
        const s = this.shadows[k];
        s.sp = ctl[b + K.SP]; s.a = ctl[b + K.A]; s.pc = ctl[b + K.RPC]; s.bp = ctl[b + K.BP]; s.mode = ctl[b + K.GW] >> 1;
        s.cycle = ctl[b + K.CYCLES];
        s.symbols = this.m0.symbols;
        s.guardHit(ctl[b + K.ADDR], ctl[b + K.GN], (ctl[b + K.GW] & 1) !== 0, ctl[b + K.VAL], ctl[b + K.OP], ctl[b + K.GFROM]);
        rstop = STOP.HALT;
      } else {
        // A device register, for a task in user mode there: what the
        // device reads or writes at the task's word is checked against
        // the task's space, as on its own CPU (mmu.js dmaCheck)
        const ar = this.m0.arena, ptbr = ctl[b + K.PTBR];
        if (ptbr) {
          if (!this.dma) this.dma = new DmaSpace(ar);
          this.dma.ptbr = ptbr;
          ar.dmaCtx = this.dma;
        }
        try {
          if (kind === KIND.RD32) ra = this.m0.bus.read32(ctl[b + K.ADDR]);
          else if (kind === KIND.WR32) this.m0.bus.write32(ctl[b + K.ADDR], ctl[b + K.VAL]);
          else if (kind === KIND.WR8) this.m0.bus.write8(ctl[b + K.ADDR], ctl[b + K.VAL]);
        } catch (e) {
          if (e !== FAULT) throw e;
          rstop = DMA_FAULT;
          ctl[b + K.FADDR] = this.dma.faddr; ctl[b + K.FINFO] = this.dma.finfo;
        } finally {
          ar.dmaCtx = null;
        }
      }
      ctl[b + K.RA] = ra;
      ctl[b + K.RSTOP] = rstop;
      Atomics.store(ctl, b + K.REQ, R.DONE);
      Atomics.notify(ctl, b + K.REQ);
    }
  }

  // Some other CPU is running, not parked
  othersRunning() {
    for (let k = 1; k < this.n; k++) if (this.state(k) === S.RUN) return true;
    return false;
  }

  // An IPI waits for CPU 0
  ipiFor0() { return Atomics.load(this.ctl, word(0, K.IPI)) !== 0; }

  // CPU 0 napped (USLP) while other CPUs run: the nap is real time, ended
  // early by an IPI, and the others' requests are answered meanwhile --
  // blocking, in Atomics.wait, which Node's main thread and a page's
  // worker both allow. An awaited wake costs a trip through the event
  // loop, a third of a millisecond: a compile on CPU 1 asks CPU 0 for
  // 13,000 mallocs and frees, and took 4 s longer for it. At most MOST
  // ms at a time, so the host's own events are not kept waiting. The ms
  // it lasted.
  sleepSync(ms, most = 10) {
    const ctl = this.ctl, t0 = Date.now(), end = Math.min(ms, most);
    for (;;) {
      const left = end - (Date.now() - t0);
      if (left <= 0 || this.ipiFor0() || Atomics.load(ctl, C.HALT)) break;
      Atomics.wait(ctl, C.DOOR, this.seenDoor, left);
      this.service();
    }
    return Date.now() - t0;
  }

  // Every other CPU halted or never started (so CPU 0 halting ends it)
  othersDone() {
    for (let k = 1; k < this.n; k++) { const s = this.state(k); if (s === S.RUN || s === S.WAIT) return false; }
    return true;
  }

  // CPU 0 parked (CWAI) or halted (CPUH): wait until it should look
  // again -- its word changed or was CWAKed, an IPI came, or the machine
  // stopped -- answering the others' requests as they ask, at most MS ms,
  // so the host's own events are not kept waiting: the caller yields and
  // comes back. It blocks, in Atomics.wait on the doorbell, as sleepSync
  // does, because an awaited wait answered each request a trip through
  // the event loop late. CPU 0 parks on the kernel lock whenever another
  // CPU holds it, and top on CPU 1 holds it for every byte it prints,
  // each a request: a game on CPU 0 stalled once a second. (ROTT's demo 4
  // under C4IX on two CPUs, cli.js: 16 s alone, 25-143 s beside top.)
  parkSync(ms = 10) {
    const m0 = this.m0, ctl = this.ctl, i32 = this.ai32, t0 = Date.now(), wk = word(0, K.WOKE);
    const woken = () => Atomics.load(ctl, wk) !== 0 || Atomics.load(ctl, word(0, K.IPI)) !== 0 ||
      Atomics.load(ctl, C.HALT) !== 0 ||
      (m0.cpuState === 'wait' && Atomics.load(i32, m0.waitAddr >> 2) !== m0.waitVal);
    let w = false;
    for (;;) {
      this.service();
      if ((w = woken())) break;
      const left = ms - (Date.now() - t0);
      if (left <= 0) break;
      Atomics.wait(ctl, C.DOOR, this.seenDoor, left);
    }
    this.idleCycles += (Date.now() - t0) * this.dev.cyclesPerMs;
    Atomics.store(ctl, wk, 0);
    super.poll(m0);
    if (m0.cpuState === 'wait' && w) {
      // CWAI's caller re-tests, as on c4mp, so a wake that was not for
      // this word costs only a look
      m0.cpuState = 'run';
      Atomics.store(ctl, word(0, K.WAITADDR), 0);
      Atomics.store(ctl, word(0, K.STATE), S.RUN);
    }
  }

  // While CPU 0 is not running (paced, or waiting for a key): answer
  // the others as soon as they ask. onAsk runs after each answer.
  watch(onAsk) {
    if (this.watching) return;
    this.watching = true;
    const r = Atomics.waitAsync(this.ctl, C.DOOR, this.seenDoor);
    const p = r.value instanceof Promise ? r.value : Promise.resolve(r.value);
    p.then(() => { this.watching = false; this.service(); if (onAsk) onAsk(); });
  }

  // CPU 0 and every other CPU parked or halted, some parked: if it stays
  // so, nothing can wake them
  allParked() {
    if (this.m0.cpuState !== 'wait' && this.m0.cpuState !== 'halt') return false;
    let waiting = this.m0.cpuState === 'wait';
    for (let k = 1; k < this.n; k++) {
      const s = this.state(k);
      if (s === S.RUN) return false;
      if (s === S.WAIT) waiting = true;
    }
    for (let k = 1; k < this.n; k++) if (Atomics.load(this.ctl, word(k, K.REQ)) === R.ASK) return false;
    return waiting;
  }

  // smp.js's report, from what each CPU left in its words
  deadlock() {
    const ctl = this.ctl, hex = v => '0x' + (v >>> 0).toString(16).toUpperCase();
    let s = 'libjs: deadlock: every processor is parked and none can be woken\n';
    for (let k = 0; k < this.n; k++) {
      const st = k === 0 ? { run: S.RUN, wait: S.WAIT, halt: S.HALT, off: S.OFF }[this.m0.cpuState] : this.state(k);
      s += `  cpu ${k}: `;
      if (st === S.OFF) s += 'off';
      else if (st === S.RUN) s += 'running';
      else if (st === S.WAIT) s += `waiting on ${hex(ctl[word(k, K.WAITADDR)])} for it to stop being ${ctl[word(k, K.WAITVAL)]}`;
      else s += 'halted';
      s += `, pc ${hex(k === 0 ? this.m0.pc : ctl[word(k, K.PC)])}, ${k === 0 ? this.m0.cycle : ctl[word(k, K.CYCLES)]} cycles\n`;
    }
    this.m0.emitString(s);
    this.dev.halted = true;
    this.dev.status = -1;
  }

  // The machine stopped: every CPU's thread leaves
  shutdown() {
    const ctl = this.ctl;
    Atomics.store(ctl, C.HALT, 1);
    for (let k = 1; k < this.n; k++) {
      Atomics.notify(ctl, word(k, K.STATE));
      Atomics.notify(ctl, word(k, K.REQ));
      Atomics.notify(ctl, word(k, K.SLEEP));
      const wa = Atomics.load(ctl, word(k, K.WAITADDR));
      if (wa) Atomics.notify(this.ai32, wa >> 2);
    }
    setTimeout(() => { for (const w of this.workers) try { w.terminate(); } catch { /* gone */ } }, 50);
  }
}

// c4m.js - the c4m virtual machine as a direct interpreter.
//
// The reference is c4m.c's dispatch loop (c4m.c:1936-2396). This is the
// same loop over an Int32Array: registers are byte addresses into one
// arena, the hot opcodes run inline, and the rare ones (syscalls, traps,
// TLEV) go through sys(), which works on the register fields.
//
// Shared with src/c4bb (imported, never edited -- an embedder vendors
// those files): the arena and its memory map, the device registers,
// the disk controller and keyboard, the clocks and the mailbox. The
// property names on Machine (cycle, trapHandler, cycleInterval, mode,
// ...) are the ones src/c4bb/sim/devices.js reads, so the devices work
// unchanged against this machine.
//
// Pure: no DOM, no Node. It runs in a page, a Worker, and node alike.

import {
  OPNAMES, UART_TX, TIME_MS, USLP_US, DISK_CLOSE,
  C4I_C4M, C4I_HRT, C4I_SIG, C4I_FLT, C4I_PROT, C4I_TRAPH, C4I_PIT, C4I_MBOX, C4I_DRV,
} from '../src/c4bb/sim/devices.js';
import { TLEV_ADDR, OPNAMES_ROM } from '../src/c4bb/sim/arena.js';
import { fpu, C4I_FPU } from '../src/c4bb/sim/fpu.js';
import { format } from './printf.js';

// Opcode numbers (c4m.c:339-390). Literal so the switch below compiles
// to a jump table; checked against the ROM names at load.
export const OP = {
  LEA: 0, IMM: 1, JMP: 2, JSR: 3, BZ: 4, BNZ: 5, ENT: 6, ADJ: 7,
  LEV: 8, LI: 9, LC: 10, SI: 11, SC: 12, PSH: 13,
  OR: 14, XOR: 15, AND: 16, EQ: 17, NE: 18, LT: 19, GT: 20, LE: 21, GE: 22,
  SHL: 23, SHR: 24, ADD: 25, SUB: 26, MUL: 27, DIV: 28, MOD: 29,
  OPEN: 30, READ: 31, CLOS: 32, PRTF: 33, MALC: 34, FREE: 35, MSET: 36, MCMP: 37, EXIT: 38,
  PUTC: 39, PUTS: 40, RALC: 41, MCPY: 42, STRC: 43, ITH: 44, _OPC: 45, _BLT: 46, _TRP: 47, OPCD: 48,
  _JMP: 49, _ADJ: 50, C4CF: 51, C4CY: 52, TIME: 53, SIGH: 54, SIGI: 55, USLP: 56, INFO: 57, OPSL: 58,
  C4IV: 59, FLT: 60, JSRI: 61, JSRS: 62, JMPA: 63, TLEV: 64, DBG: 65,
  // 66-75 are c4mp's processor opcodes, run when the machine has CPUs
  // (smp.js); 76-78 (LXI, SXI, TRAW) are c4mp's alone
  CPUI: 66, CPUN: 67, CPUS: 68, CPUH: 69, CAS: 70, XCHG: 71, FADD: 72, CWAI: 73, CWAK: 74, IPI: 75,
  LDL: 79, LDG: 80, PSHL: 81, PSHG: 82, LEAP: 83, IMMP: 84, LIP: 85, ADDL: 86, STL: 87, POPA: 88, FPU: 89,
};
export const INS_SIZE = 89;
OPNAMES.match(/.{5}/g).forEach((n, i) => {
  const name = n.slice(0, 4).trim();
  if (OP[name] !== undefined && OP[name] !== i)
    throw new Error(`libjs: opcode ${name} is ${i} in the ROM, ${OP[name]} here`);
});

// c4m_has_operand (c4m.c:420-424), including negative numbers, which
// c4m's `i <= ADJ` also counts.
export function hasOperand(i) {
  return i <= OP.ADJ || i === OP.JSRI || i === OP.JSRS ||
         (i >= OP.LDL && i <= OP.IMMP) || i === OP.STL;
}

// The ROM a guest never stores to (src/c4bb/sim/arena.js): below 0x20
// the null guard and the TLEV, constructor-return and CPUH words; and
// the opcode-name ROM. c4bb's firmware latches at 0x20-0x3F are RAM.
// Does the range of n bytes at x touch it?
const ROM_LOW_END = 0x20;
function isRom(x, n) {
  const lo = x | 0, hi = lo + n;
  return (lo < ROM_LOW_END && hi > 0) ||
         (lo < OPNAMES_ROM + OPNAMES.length && hi > OPNAMES_ROM);
}

// run()'s own opcodes that OPCD can run, which have no operand: opcd().
function isPlainOp(i) {
  return (i >= OP.LEV && i <= OP.MOD) || i === OP._JMP || i === OP._ADJ || i === OP.C4CY ||
         i === OP.JMPA || i === OP.LIP || i === OP.ADDL || i === OP.POPA;
}

export const TRAP = { ILLOP: 0, HARD_IRQ: 1, SOFT_IRQ: 2, SIGNAL: 3, SEGV: 4, OPV: 5, PM_VIOLATION: 6, DEBUG: 7 };
const TRAP_OFFSET = 15;                 // words of slack above a trap frame (c4m.c:221)

// Capability bits beyond c4bb's. C4I_C4MJS is reserved in c4m.c:277 for
// exactly this host. C4I_GUI announces the display device (gui-device.js),
// C4I_SND the sound card (snd-device.js).
export const C4I_C4MJS = 0x8, C4I_GUI = 0x4000, C4I_SND = 0x8000;
// c4mp's: the processor opcodes and the CPU-local window are there. The
// count is __c4_cpu_count()'s to say. (c4m.c gives 0x100 to its memory
// protection guard too, which libjs never runs.)
export const C4I_SMP = 0x100;
// ... and they run at once, a thread each (par.js), rather than taking
// turns on one (smp.js). Free in c4m.c's list, which ends at C4I_FPU.
export const C4I_PAR = 0x20000;

// Why run() stopped.
// PARK and CPUHALT are one processor's, never the machine's (smp.js).
export const STOP = { BUDGET: 0, HALT: 1, INPUT: 2, SLEEP: 3, PARK: 4, CPUHALT: 5 };
export const STOP_NAMES = ['budget', 'halt', 'input', 'sleep', 'park', 'cpuhalt'];

// SMP (smp.js, c4mp's instruction set): where a started CPU's entry
// function returns -- a CPUH word in the ROM -- and the CPU-local window,
// 64 words each processor sees its own of (docs/c4ix-smp.md, 3.4)
export const CPU_HALT_ADDR = 0x18;
export const LOCAL_BASE = 0x800, LOCAL_END = 0x900;
// HARD_IRQ's sub-code for an inter-processor interrupt: c4mp's HIRQ_IPI.
// The PIT's tick is 1 too; no SMP guest runs the PIT.
const HIRQ_IPI = 1;

export class Machine {
  // arena: src/c4bb/sim/arena.js Arena; dev: src/c4bb/sim/devices.js
  // Devices; bus: bus.js Bus (low memory); heap: heap.js Heap.
  constructor(arena, dev, bus, heap = null) {
    this.arena = arena;
    this.dev = dev;
    this.bus = bus;
    this.heap = heap;
    this.gui = bus.gui || null;
    this.snd = bus.snd || null;
    dev.machine = this;
    arena.io = dev;
    this.i32 = arena.i32;
    this.u8 = arena.u8;
    // On the parallel engine the arena is a SharedArrayBuffer: what leaves
    // the machine (strings to print, to decode) is copied, not a view
    this.sharedMem = typeof SharedArrayBuffer !== 'undefined' && arena.u8.buffer instanceof SharedArrayBuffer;
    this.reset();
  }

  reset() {
    this.pc = 0; this.sp = 0; this.bp = 0; this.a = 0;
    this.mode = 0;
    this.cycle = 0;
    this.skipped = 0;                   // cycles fast-forwarded past a blocked read
    this.trapHandler = 0;
    this.cycleInterval = 0;
    this.cycleHandler = 0;
    this.bFired = false; this.bNextIrq = 0;   // boundary()'s results
    this.trapRestoresInterval = 0;
    this.signalHandlers = new Map();
    this.pendingSignal = 0;
    this.missedTraps = 0;
    this.lastTrap = null;
    this.symbols = this.symbols || [];
    // c4bb's trap-jam latches, read through device registers. A direct
    // interpreter has no jam, so they stay zero.
    this.tt = 0; this.tp = 0; this.hnd = 0; this.jmode = 0; this.jinterval = 0;
    // Trap handlers in progress: +1 at each trap that enters one, -1 at
    // each TLEV. The PIT and the mailbox are held while it is non-zero,
    // as by c4bb's irqHold (machine.js) -- see boundary().
    this.irqHold = 0;
  }

  // ---------------------------------------------------------------
  // Memory, slow paths. The loop inlines the aligned, above-0x1000 case
  // and comes here for everything else: the device window, the ROM,
  // unaligned words, and addresses off either end of the arena.
  ld32(x) {
    x = x | 0;
    if ((x & 3) === 0 && x >= 0x1000) return this.i32[x >> 2] | 0;
    if (this.smp && x >= LOCAL_BASE && x < LOCAL_END && (x & 3) === 0) return this.local[(x - LOCAL_BASE) >> 2] | 0;
    return this.bus.read32(x) | 0;
  }
  st32(x, v) {
    x = x | 0;
    if ((x & 3) === 0 && x >= 0x1000) { this.i32[x >> 2] = v; return; }
    if (this.smp && x >= LOCAL_BASE && x < LOCAL_END && (x & 3) === 0) { this.local[(x - LOCAL_BASE) >> 2] = v; return; }
    if (isRom(x, 4)) { this.romStore(x, v); return; }
    this.bus.write32(x, v | 0);
  }
  ld8(x) {
    x = x | 0;
    if (x >= 0x1000) return (this.u8[x] << 24) >> 24;
    if (this.smp && x >= LOCAL_BASE && x < LOCAL_END) return (this.localU8[x - LOCAL_BASE] << 24) >> 24;
    return this.bus.read8s(x);
  }
  st8(x, v) {
    x = x | 0;
    if (x >= 0x1000) { this.u8[x] = v; return; }
    if (this.smp && x >= LOCAL_BASE && x < LOCAL_END) { this.localU8[x - LOCAL_BASE] = v; return; }
    if (isRom(x, 1)) { this.romStore(x, v & 0xff); return; }
    this.bus.write8(x, v & 0xff);
  }

  // A store into the ROM words: dropped, and the first one named on the
  // console. Every trap in every task returns through the TLEV word, so
  // one wild store there ends the whole machine.
  romStore(x, v) {
    this.romStores = (this.romStores | 0) + 1;
    const dev = this.dev;
    if (dev.romReported) return;
    dev.romReported = true;
    const hex = n => '0x' + ((n >>> 0).toString(16).toUpperCase().padStart(8, '0'));
    const pc = this.pc - 4;
    this.emitString(`libjs: store of ${hex(v)} to ROM ${hex(x)} at pc ${hex(pc)} dropped` +
      ` (bp ${hex(this.bp)}, sp ${hex(this.sp)}, mode ${this.mode}, cycle ${this.cycle}).` +
      ' Later ones are dropped silently.\n');
  }

  // READ into a buffer over the ROM. The disk controller writes the
  // arena itself, so the ROM is put back afterwards.
  romRead(len) {
    const u8 = this.u8, end = OPNAMES_ROM + OPNAMES.length;
    const saved = u8.slice(0, end);
    const r = this.dev.diskRead(len) | 0;
    for (let x = 0; x < end; x++) {
      if (x === ROM_LOW_END) x = OPNAMES_ROM;
      if (u8[x] !== saved[x]) { this.romStore(x, u8[x]); u8[x] = saved[x]; }
    }
    return r;
  }

  // Bytes into memory from the host side of an opcode (MCPY), a range
  // that may cover the ROM: store each through st8's check.
  stBytes(d, src, n) {
    const u8 = this.u8;
    for (let k = 0; k < n; k++) {
      const x = (d + k) | 0;
      if (isRom(x, 1)) this.romStore(x, src(k)); else u8[x] = src(k);
    }
  }
  cstring(addr, max = 1 << 20) {
    const u8 = this.u8;
    addr = addr >>> 0;
    let end = addr;
    const lim = Math.min(u8.length, addr + max);
    while (end < lim && u8[end]) end++;
    return this.sharedMem ? u8.slice(addr, end) : u8.subarray(addr, end);
  }

  // Bytes to the console, in order with everything else the UART sends.
  emit(bytes) {
    const dev = this.dev, f = dev.onByte;
    dev.uartActivity += bytes.length;
    if (f) for (let i = 0; i < bytes.length; i++) f(bytes[i]);
  }
  emitString(s) {
    const b = new TextEncoder().encode(s);
    this.emit(b);
  }

  info() {
    return C4I_C4M | C4I_C4MJS | C4I_HRT | C4I_SIG | C4I_FLT | C4I_PROT | C4I_PIT | C4I_DRV |
           (this.noFpu ? 0 : C4I_FPU) |
           (this.dev.mbox ? C4I_MBOX : 0) |
           (this.gui && this.gui.fitted ? C4I_GUI : 0) |
           (this.snd && this.snd.fitted ? C4I_SND : 0) |
           (this.smp ? C4I_SMP : 0) |
           (this.smp && this.smp.parallel ? C4I_PAR : 0) |
           (this.trapHandler ? C4I_TRAPH : 0);
  }

  // ---------------------------------------------------------------
  // Traps: c4m.c:1445-1516, on byte addresses. Returns false when there
  // is no handler (a missed trap), in which case nothing moves -- the
  // call site's own effects still apply, as they do in c4m.
  trap(type, param, handler) {
    handler = handler | 0;
    if (!handler) {
      this.missedTraps++;
      this.lastTrap = { type, param, cycle: this.cycle };
      return false;
    }
    const i32 = this.i32;
    const t = this.sp;
    let sp = t - TRAP_OFFSET * 4;
    i32[(sp -= 4) >> 2] = this.cycleInterval;   // bp+9
    this.cycleInterval = 0;
    i32[(sp -= 4) >> 2] = type;                 // bp+8
    i32[(sp -= 4) >> 2] = param;                // bp+7
    i32[(sp -= 4) >> 2] = this.mode;            // bp+6
    i32[(sp -= 4) >> 2] = this.a;               // bp+5
    i32[(sp -= 4) >> 2] = this.bp;              // bp+4
    i32[(sp -= 4) >> 2] = t;                    // bp+3
    i32[(sp -= 4) >> 2] = this.pc;              // bp+2
    i32[(sp -= 4) >> 2] = TLEV_ADDR;            // bp+1: LEV returns to TLEV
    sp -= 4;
    i32[sp >> 2] = sp;                          // bp+0: self
    this.bp = sp;
    sp -= this.ld32(handler + 4) * 4;           // the handler's ENT operand
    this.sp = sp;
    this.pc = handler + 8;                      // past ENT n
    this.irqHold++;
    return true;
  }

  // The cycle interrupt, the PIT and the mailbox: HARD_IRQ with its
  // source as the parameter, and the site's effects.
  hirq(param) {
    this.trap(TRAP.HARD_IRQ, param, this.cycleHandler);
    this.cycleInterval = 0;
    this.mode = 0;
  }

  deliverSignal() {
    const sig = this.pendingSignal;
    this.trap(TRAP.SIGNAL, sig, this.signalHandlers.get(sig) | 0);
    this.cycleInterval = 0;
    this.mode = 0;
    this.pendingSignal = 0;
  }

  // A syscall opcode run in protected mode.
  pmTrap(op) {
    this.trap(TRAP.PM_VIOLATION, op, this.trapHandler);
    this.cycleInterval = 0;
    this.mode = 0;
    return STOP.BUDGET;
  }

  // EXIT. With CPUs, only CPU 0's ends the machine, as main returning
  // ends a program; another's halts that processor (c4mp's rule).
  halt(status) {
    if (this.smp && this.cpuId !== 0) {
      this.cpuState = 'halt';
      this.cpuStatus = status | 0;
      return STOP.CPUHALT;
    }
    this.dev.halted = true;
    this.dev.status = status | 0;
    return STOP.HALT;
  }

  // c4mp's processor opcodes (66-75), when the machine has CPUs. Their
  // arguments are on the stack as a syscall's are, the last pushed on
  // top; c4lc emits the ADJ after. Unguarded in protected mode, as c4mp
  // has them. In the simulated engine an instruction cannot be split,
  // so the atomics are plain read-modify-writes.
  smpOp(op) {
    const sp = this.sp, smp = this.smp;
    // the parallel engine (par.js): real threads, real Atomics
    if (smp.parallel) return smp.op(this, op);
    switch (op) {
      case OP.CPUI: this.a = this.cpuId; return STOP.BUDGET;
      case OP.CPUN: this.a = smp.n; return STOP.BUDGET;
      case OP.CPUS: this.a = smp.start(this.ld32(sp + 8), this.ld32(sp + 4), this.ld32(sp)); return STOP.BUDGET;
      case OP.CPUH: this.cpuState = 'halt'; return STOP.CPUHALT;
      case OP.CAS: {
        const t = this.ld32(sp + 8), v = this.ld32(t);
        if (v === this.ld32(sp + 4)) this.st32(t, this.ld32(sp));
        this.a = v;
        return STOP.BUDGET;
      }
      case OP.XCHG: {
        const t = this.ld32(sp + 4), v = this.ld32(t);
        this.st32(t, this.ld32(sp));
        this.a = v;
        return STOP.BUDGET;
      }
      case OP.FADD: {
        const t = this.ld32(sp + 4), v = this.ld32(t);
        this.st32(t, (v + this.ld32(sp)) | 0);
        this.a = v;
        return STOP.BUDGET;
      }
      case OP.CWAI: {
        // park while *addr is still val: 1 if it already is not, 0 once woken
        const t = this.ld32(sp + 4), val = this.ld32(sp);
        if (!t || this.ld32(t) !== val) { this.a = 1; return STOP.BUDGET; }
        this.a = 0;
        this.waitAddr = t; this.waitVal = val;
        this.cpuState = 'wait';
        return STOP.PARK;
      }
      case OP.CWAK: this.a = smp.wake(this.ld32(sp + 4), this.ld32(sp)); return STOP.BUDGET;
      case OP.IPI: this.a = smp.ipi(this.ld32(sp)); return STOP.BUDGET;
    }
    return STOP.BUDGET;
  }

  // ---------------------------------------------------------------
  // The rare opcodes, on the register fields. Returns a STOP code;
  // BUDGET means carry on.
  sys(op) {
    const dev = this.dev, sp = this.sp;
    if (op >= OP.CPUI && op <= OP.IPI && this.smp) return this.smpOp(op);
    switch (op) {
      case OP.OPEN:
        if (this.mode) return this.pmTrap(op);
        dev.diskFlags = this.ld32(sp);
        this.a = dev.diskOpen(this.ld32(sp + 4)) | 0;
        return STOP.BUDGET;
      case OP.READ: {
        if (this.mode) return this.pmTrap(op);
        dev.diskFd = this.ld32(sp + 8);
        dev.diskAddr = this.ld32(sp + 4);
        const r = isRom(dev.diskAddr, this.ld32(sp)) ? this.romRead(this.ld32(sp))
                                                     : dev.diskRead(this.ld32(sp)) | 0;
        if (r === -2) return this.blockedRead();
        this.a = r;
        return STOP.BUDGET;
      }
      case OP.CLOS:
        if (this.mode) return this.pmTrap(op);
        dev.write32(DISK_CLOSE, this.ld32(sp));
        this.a = dev.read32(DISK_CLOSE) | 0;
        return STOP.BUDGET;
      case OP.PRTF: {
        if (this.mode) return this.pmTrap(op);
        // c4m.c:2124-2143: the argument count is the operand of the ADJ
        // that follows PRTF; the format is the deepest word pushed.
        const r = this.ld32(this.pc + 4);
        if (r > 7) {
          this.emitString('Too many arguments to printf!\n');
          return this.halt(-1);
        }
        const t = sp + r * 4;
        const args = [];
        for (let k = 2; k <= 7; k++) args.push(this.ld32(t - k * 4));
        const out = format(this.cstring(this.ld32(t - 4)), args, p => this.cstring(p));
        this.emit(out);
        this.a = out.length;
        return STOP.BUDGET;
      }
      case OP.MALC:
        if (this.mode) return this.pmTrap(op);
        this.a = this.heap.alloc(this.ld32(sp));
        return STOP.BUDGET;
      case OP.FREE:
        if (this.mode) return this.pmTrap(op);
        this.heap.free(this.ld32(sp));
        return STOP.BUDGET;
      case OP.RALC:
        if (this.mode) return this.pmTrap(op);
        this.a = this.heap.realloc(this.u8, this.ld32(sp + 4), this.ld32(sp));
        return STOP.BUDGET;
      case OP.MSET: {
        const d = this.ld32(sp + 8), v = this.ld32(sp + 4), n = this.ld32(sp);
        if (n > 0) {
          if (d >= 0x1000) this.u8.fill(v & 0xff, d, d + n);
          else for (let k = 0; k < n; k++) this.st8(d + k, v);
        }
        this.a = d;
        return STOP.BUDGET;
      }
      case OP.MCMP: {
        // glibc: the difference of the first unequal bytes, unsigned
        const p = this.ld32(sp + 8), q = this.ld32(sp + 4), n = this.ld32(sp);
        const u8 = this.u8;
        let r = 0;
        for (let k = 0; k < n; k++) {
          const x = (u8[p + k] | 0) - (u8[q + k] | 0);
          if (x) { r = x; break; }
        }
        this.a = r;
        return STOP.BUDGET;
      }
      case OP.MCPY: {
        const d = this.ld32(sp + 8), s = this.ld32(sp + 4), n = this.ld32(sp);
        if (n > 0) {
          const u8 = this.u8;
          // Forward, byte by byte, when the ranges overlap that way --
          // what c4bb's microcode and a naive memcpy do. Otherwise a
          // block copy.
          if (isRom(d, n)) this.stBytes(d, k => u8[s + k], n);
          else if (d > s && d < s + n) for (let k = 0; k < n; k++) u8[d + k] = u8[s + k];
          else u8.copyWithin(d, s, s + n);
        }
        this.a = d;
        return STOP.BUDGET;
      }
      case OP.EXIT:
        if (this.mode) return this.pmTrap(op);
        return this.halt(this.ld32(sp));
      case OP.PUTC: {
        if (this.mode) return this.pmTrap(op);
        const b = this.u8[sp];                  // *(char *)sp: the low byte
        this.emit([b]);
        this.a = b;
        return STOP.BUDGET;
      }
      case OP.PUTS: {
        if (this.mode) return this.pmTrap(op);
        this.emit(this.cstring(this.ld32(sp)));
        this.emit([10]);
        this.a = 10;
        return STOP.BUDGET;
      }
      case OP.STRC:
        this.stacktrace();
        return STOP.BUDGET;
      case OP.ITH: {
        // guarded: the trap handler is the machine's, the kernel's
        if (this.mode) return this.pmTrap(op);
        const old = this.trapHandler;
        this.trapHandler = this.ld32(sp);
        this.a = old;
        return STOP.BUDGET;
      }
      case OP._OPC: {
        const name = new TextDecoder('latin1').decode(this.cstring(this.ld32(sp), 16));
        this.a = dev.opcodeByName(name) | 0;
        return STOP.BUDGET;
      }
      case OP._TRP:
        // __c4_trap(type, param): the site touches neither interval nor mode
        this.trap(this.ld32(sp + 4), this.ld32(sp), this.trapHandler);
        return STOP.BUDGET;
      case OP.C4CF: {
        const opt = this.ld32(sp + 4), val = this.ld32(sp);
        if (opt === 0) { this.a = this.cycleInterval; this.cycleInterval = val; }
        else if (opt === 1) { this.a = this.cycleHandler; this.cycleHandler = val; }
        else if (opt === 3) { this.a = this.trapRestoresInterval; this.trapRestoresInterval = val; }
        else { this.emitString('c4m: C4CF issue\n'); return this.halt(-100); }
        return STOP.BUDGET;
      }
      case OP.TIME:
        this.a = dev.read32(TIME_MS) | 0;
        return STOP.BUDGET;
      case OP.SIGH: {
        // guarded: it sets the machine's handler, which is the kernel's
        if (this.mode) return this.pmTrap(op);
        const sig = this.ld32(sp + 4), h = this.ld32(sp);
        this.a = this.signalHandlers.get(sig) | 0;
        this.signalHandlers.set(sig, h);
        return STOP.BUDGET;
      }
      case OP.SIGI:
        this.a = 2;
        return STOP.BUDGET;
      case OP.USLP:
        // guarded: it sleeps the whole machine, not just the caller
        if (this.mode) return this.pmTrap(op);
        dev.write32(USLP_US, this.ld32(sp));
        this.a = 0;
        return STOP.SLEEP;
      case OP.INFO:
        if (this.mode) return this.pmTrap(op);
        this.a = this.info();
        return STOP.BUDGET;
      case OP.OPSL:
        this.a = OPNAMES_ROM;
        return STOP.BUDGET;
      case OP.C4IV:
        // Native c4m does nothing here: the invoke stub only means
        // something when c4m is itself interpreted by plain c4.
        return STOP.BUDGET;
      case OP.FLT:
        this.a = this.flt(sp);
        return STOP.BUDGET;
      case OP.FPU: {
        // F3 (src/c4bb/sim/fpu.js); with the FPU switched off, unknown
        // here like any opcode this machine lacks
        const v = this.noFpu ? undefined : fpu(this, sp);
        if (v !== undefined) { this.a = v | 0; return STOP.BUDGET; }
        break;
      }
      case OP.TLEV: {
        if (this.irqHold > 0) this.irqHold--;   // the handler ends: see boundary()
        const bp = this.bp;
        const pc = this.ld32(bp + 8);
        if (pc === this.pc - 4) return this.tlevWedge(pc);
        this.sp = this.ld32(bp + 12);
        this.bp = this.ld32(bp + 16);
        this.a = this.ld32(bp + 20);
        this.mode = this.ld32(bp + 24);
        if (this.trapRestoresInterval) this.cycleInterval = this.ld32(bp + 36);
        this.pc = pc;
        return STOP.BUDGET;
      }
      case OP.DBG:
        this.trap(TRAP.DEBUG, 0, this.trapHandler);
        return STOP.BUDGET;
    }
    if (op >= 0 && op < INS_SIZE && !this.trapHandler) {
      // Named but not implemented here (_BLT, c4mp's 66-78): a machine
      // mismatch, not a custom opcode. c4m halts (c4m.c:2360-2383).
      const name = OPNAMES.slice(op * 5, op * 5 + 4);
      const at = ((this.pc - 4) >>> 0).toString(16).toUpperCase();
      this.emitString(`c4m: ${name} (opcode ${op}) at 0x${at} is not an instruction this machine has,\n`);
      this.emitString('c4m: and no trap handler is installed to emulate it. Halting.\n');
      return this.halt(-1);
    }
    // An unknown opcode: TRAP_ILLOP, the custom-opcode convention both
    // kernels build their syscalls on. The site only drops protection.
    this.trap(TRAP.ILLOP, op, this.trapHandler);
    this.mode = 0;
    return STOP.BUDGET;
  }

  // READ said "would block" (-2, c4bb's convention). The task retries the
  // same READ until input arrives, and meanwhile the machine must keep
  // taking interrupts so the kernel can run everyone else. Re-executing a
  // READ that returns -2 changes nothing, so rather than spin one cycle
  // at a time the clock jumps to the next cycle interrupt: exactly the
  // state a spin would have reached, without the host burning a core.
  blockedRead() {
    this.pc -= 4;
    const dev = this.dev;
    const iv = this.cycleInterval;
    if (iv > 0) {
      const target = (Math.floor(this.cycle / iv) + 1) * iv - 1;
      if (target > this.cycle) { this.skipped += target - this.cycle; this.cycle = target; }
      return STOP.BUDGET;
    }
    if (dev.pitMs && dev.hostNow() >= dev.pitNext) {
      // The PIT is looked at on 4096-cycle boundaries: go to the next one.
      const target = this.cycle + (4095 - (this.cycle % 4096));
      if (target > this.cycle) { this.skipped += target - this.cycle; this.cycle = target; }
      return STOP.BUDGET;
    }
    return STOP.INPUT;
  }

  // STRC: walk the frame chain from here to main, naming each function
  // from the image's symbol section (boot.js parseSymbols). Each frame's
  // bp holds the caller's bp and, one word up, the return address.
  stacktrace() {
    const syms = this.symbols || [];
    const hex = n => '0x' + ((n >>> 0).toString(16).toUpperCase());
    const find = pc => {
      let lo = 0, hi = syms.length - 1, best = null;
      while (lo <= hi) {
        const m = (lo + hi) >> 1;
        if (syms[m].addr <= pc) { best = syms[m]; lo = m + 1; } else hi = m - 1;
      }
      return best;
    };
    let pc = this.pc - 4, bp = this.bp;
    this.emitString('c4m: stacktrace:\n');
    for (let depth = 0; depth < 64; depth++) {
      const s = find(pc);
      this.emitString(`${' '.repeat(depth + 1)}${s ? s.name : '?'}() [${s ? hex(s.addr) : hex(pc)}]\n`);
      if (!s || s.name === 'main' || bp < 0x1000 || bp >= this.arena.size) break;
      pc = this.ld32(bp + 4) - 4;
      bp = this.ld32(bp);
    }
  }

  tlevWedge(pc) {
    const hex = n => '0x' + ((n >>> 0).toString(16).toUpperCase().padStart(8, '0'));
    this.emitString(`libjs: TLEV at ${hex(pc)} restores its own address -- this task can never advance.\n`);
    this.emitString(`libjs:   bp ${hex(this.bp)}, sp ${hex(this.sp)}, a ${hex(this.a)}, after ${this.cycle} cycles.\n`);
    this.emitString('libjs:   A trap frame was built wrong or written over. Halting.\n');
    return this.halt(-101);
  }

  // c4_float_instruction (c4m_float.c:31): binary32 values in words.
  flt(sp) {
    const f = Machine.f32, fi = Machine.fi32;
    const flt = i => { fi[0] = i; return f[0]; };
    const op = this.ld32(sp);
    const a = flt(this.ld32(sp + 4));
    const b = flt(this.ld32(sp + 8));
    let c;
    switch (op) {
      case 0: c = a < 0 ? -a : a; break;          // ABSF
      case 1: c = a + b; break;
      case 2: c = a - b; break;
      case 3: c = a * b; break;
      case 4: c = a / b; break;
      case 5: c = -a; break;
      case 6: c = Math.sin(a); break;
      case 7: c = Math.cos(a); break;
      case 8: c = Math.tan(a); break;
      case 9: c = this.ld32(sp + 4); break;       // ITOF
      case 10: return Math.trunc(a) | 0;          // FTOI: an integer, not a float's bits
      default:
        this.emitString(`c4m fatal error: unsupported floating point instruction ${op}!\n`);
        c = 0;
    }
    f[0] = c;
    return fi[0];
  }

  // Anything that makes the per-instruction boundary check necessary.
  needChecks() {
    const dev = this.dev;
    return !!(this.cycleInterval || dev.pitMs || this.pendingSignal || dev.mboxIrq || this.ipiPending ||
              (this.gui && this.gui.irqPending()));
  }

  // The instruction-boundary checks, in c4bb's order (machine.js:189-242),
  // with the registers in their fields. They are out of run()'s loop so
  // that their calls, which C4IX takes and a bare program does not, do
  // not shape the loop's code. Returns the cycle they are next due at
  // (0: the next instruction); bFired says whether one was taken, and
  // bNextIrq is the next tick.
  //
  // Neither the PIT nor the mailbox interrupts a trap handler: both are
  // held from the trap to the TLEV that ends it (irqHold), and a tick
  // due meanwhile comes at the first look after that TLEV, the deadline
  // kept. It is c4bb's rule (machine.js boundaryChecks): C4KE masks the
  // PIT for its critical path and re-arms it with a store a few
  // instructions before its TLEV, when kernel_task_current already names
  // the next task, so an overdue tick in between saved the half-finished
  // handler as that task's context, and the task looped on its TLEV for
  // ever. The gui IRQ and IPIs are not held: no kernel masks and re-arms
  // them that way, and C4IX waits for a kernel lock inside its trap
  // handler (CWAI, sched.c klock_take), which must not wait behind them.
  boundary(cycle, nextIrq) {
    const dev = this.dev, i32 = this.i32;
    const tlev = i32[this.pc >> 2] === OP.TLEV;   // TLEV is never interrupted
    const held = tlev || this.irqHold > 0;        // ... nor, by the PIT or the mailbox, a handler
    let interval = this.cycleInterval | 0;
    let fired = true;
    if (dev.pitMs && (cycle & 4095) === 0 && !held && dev.hostNow() >= dev.pitNext) {
      dev.pitNext = dev.hostNow() + dev.pitMs;
      this.hirq(1);
    } else if (dev.mboxIrq && this.cycleHandler && !held) {
      dev.mboxIrq = 0;
      this.hirq(2);
    } else if (this.gui && this.cycleHandler && this.gui.irqPending() && !tlev) {
      this.gui.irqTaken();
      this.hirq(3);
    } else {
      let irq = false;
      if (interval > 0 && cycle >= nextIrq) {
        nextIrq = (Math.floor(cycle / interval) + 1) * interval;
        irq = !tlev;
      }
      if (irq) this.hirq(0);
      else if (this.ipiPending && !tlev) {
        // another CPU's interrupt (c4mp's order: after the tick).
        // Not held back by the preemption mask: it is a peer asking.
        this.ipiPending = 0;
        this.hirq(HIRQ_IPI);
      } else if (this.pendingSignal && !(this.trapRestoresInterval && !interval)) {
        this.deliverSignal();
      } else fired = false;
    }
    this.bFired = fired;
    if (fired) {
      interval = this.cycleInterval | 0;
      this.bNextIrq = interval > 0 ? (Math.floor(cycle / interval) + 1) * interval : 0;
      return 0;
    }
    this.bNextIrq = nextIrq;
    // Pending but held back (a TLEV, a mask): look again. A mailbox
    // interrupt held for a handler need not: the TLEV that ends the hold
    // goes through sys(), after which run() looks at once.
    if ((dev.mboxIrq && this.cycleHandler && !this.irqHold) || this.ipiPending || this.pendingSignal ||
        (this.gui && this.cycleHandler && this.gui.irqPending()))
      return 0;
    let next = Infinity;
    if (interval > 0) next = nextIrq;
    if (dev.pitMs) {
      const pit = (Math.floor(cycle / 4096) + 1) * 4096;
      if (pit < next) next = pit;
    }
    return next;
  }

  // OPCD of one of run()'s own opcodes without an operand (isPlainOp),
  // on the register fields. The opcode stays on the stack, under what
  // it pushes, as it does for a syscall. Rare: C4IX's OPCDs are
  // syscalls.
  opcd(op) {
    const i32 = this.i32;
    let sp = this.sp, a = this.a, x = 0;
    switch (op) {
      case OP.LEV: {
        const bp = this.bp;
        this.bp = i32[bp >> 2]; this.pc = i32[(bp >> 2) + 1]; sp = bp + 8;
        break;
      }
      case OP.LI: a = this.ld32(a); break;
      case OP.LC: a = this.ld8(a); break;
      case OP.SI: x = i32[sp >> 2]; sp += 4; this.st32(x, a); break;
      case OP.SC: x = i32[sp >> 2]; sp += 4; this.st8(x, a); a = (a << 24) >> 24; break;
      case OP.PSH: sp -= 4; i32[sp >> 2] = a; break;
      case OP.LIP: a = this.ld32(a); sp -= 4; i32[sp >> 2] = a; break;
      case OP.ADDL: x = (i32[sp >> 2] + a) | 0; sp += 4; a = this.ld32(x); break;
      case OP.POPA: a = i32[sp >> 2]; sp += 4; break;
      case OP.JMPA: this.pc = a; break;
      case OP._JMP: this.pc = i32[sp >> 2]; sp += 4; break;
      case OP._ADJ: sp = (sp + (i32[sp >> 2] << 2)) | 0; break;
      case OP.C4CY: a = this.cycle | 0; break;
      default: {
        // OR .. MOD: the second operand is a, the first is popped
        const t = i32[sp >> 2]; sp += 4;
        switch (op) {
          case OP.OR: a = t | a; break;
          case OP.XOR: a = t ^ a; break;
          case OP.AND: a = t & a; break;
          case OP.EQ: a = t === a ? 1 : 0; break;
          case OP.NE: a = t !== a ? 1 : 0; break;
          case OP.LT: a = t < a ? 1 : 0; break;
          case OP.GT: a = t > a ? 1 : 0; break;
          case OP.LE: a = t <= a ? 1 : 0; break;
          case OP.GE: a = t >= a ? 1 : 0; break;
          case OP.SHL: a = t << a; break;
          case OP.SHR: a = t >> a; break;
          case OP.ADD: a = (t + a) | 0; break;
          case OP.SUB: a = (t - a) | 0; break;
          case OP.MUL: a = Math.imul(t, a); break;
          case OP.DIV: a = a === 0 ? 0 : (t / a) | 0; break;
          case OP.MOD: a = a === 0 ? 0 : (t % a) | 0; break;
        }
      }
    }
    this.sp = sp; this.a = a;
  }

  // ---------------------------------------------------------------
  // Run up to maxCycles cycles. Returns a STOP code.
  run(maxCycles) {
    // with CPUs, the boot CPU's run is the round robin's (smp.js)
    if (this.smp && !this.smp.running) return this.smp.run(maxCycles);
    const dev = this.dev, i32 = this.i32, u8 = this.u8;
    if (dev.halted) return STOP.HALT;
    let pc = this.pc | 0, sp = this.sp | 0, bp = this.bp | 0, a = this.a | 0;
    let cycle = this.cycle;
    const end = cycle + maxCycles;
    let interval = this.cycleInterval | 0;
    let nextIrq = interval > 0 ? (Math.floor(cycle / interval) + 1) * interval : 0;
    let chk = this.needChecks();
    // The checks are due at the next tick or PIT look, not at every
    // instruction: within a slice nothing else can become pending
    // except on a path that calls needChecks() again, which zeroes
    // this. Host events (keys, the display's, an IPI) land between
    // slices. 0: look at the next instruction.
    let nextChk = 0;
    // An instruction up to lim needs neither the checks nor a look at
    // end: one compare per instruction. lim is the smaller of end and
    // nextChk - 1; -1 makes the next instruction look at both.
    let lim = -1;
    let stop = STOP.BUDGET;
    let op = 0, x = 0;

    loop: for (;;) {
      if (++cycle > lim) {
        if (cycle > end) { --cycle; break; }
        if (chk && cycle >= nextChk) {
          this.pc = pc; this.sp = sp; this.bp = bp; this.a = a; this.cycle = cycle;
          nextChk = this.boundary(cycle, nextIrq);
          nextIrq = this.bNextIrq;
          if (this.bFired) {
            pc = this.pc; sp = this.sp; bp = this.bp; a = this.a;
            interval = this.cycleInterval | 0;
            chk = this.needChecks();
          }
        }
        lim = chk && nextChk - 1 < end ? nextChk - 1 : end;
      }

      op = i32[pc >> 2];
      pc += 4;

      {                          // the dispatch: never a loop (OPCD, below)
        switch (op) {
          case 0:  /* LEA */ a = (bp + (i32[pc >> 2] << 2)) | 0; pc += 4; break;
          case 1:  /* IMM */ a = i32[pc >> 2]; pc += 4; break;
          case 2:  /* JMP */ pc = i32[pc >> 2]; break;
          case 3:  /* JSR */ sp -= 4; i32[sp >> 2] = pc + 4; pc = i32[pc >> 2]; break;
          case 4:  /* BZ  */ pc = a ? pc + 4 : i32[pc >> 2]; break;
          case 5:  /* BNZ */ pc = a ? i32[pc >> 2] : pc + 4; break;
          case 6:  /* ENT */ sp -= 4; i32[sp >> 2] = bp; bp = sp; sp = (sp - (i32[pc >> 2] << 2)) | 0; pc += 4; break;
          case 7:  /* ADJ */ sp = (sp + (i32[pc >> 2] << 2)) | 0; pc += 4; break;
          case 8:  /* LEV */ sp = bp; bp = i32[sp >> 2]; pc = i32[(sp >> 2) + 1]; sp += 8; break;
          case 9:  /* LI  */ a = ((a & 3) === 0 && a >= 0x1000) ? i32[a >> 2] | 0 : (this.cycle = cycle, this.ld32(a)); break;
          case 10: /* LC  */ a = a >= 0x1000 ? (u8[a] << 24) >> 24 : (this.cycle = cycle, this.ld8(a)); break;
          case 11: /* SI  */
            x = i32[sp >> 2]; sp += 4;
            if ((x & 3) === 0 && x >= 0x1000) i32[x >> 2] = a;
            else {
              this.pc = pc; this.sp = sp; this.bp = bp; this.a = a; this.cycle = cycle;
              this.st32(x, a);
              if (dev.halted) { stop = STOP.HALT; break loop; }
              interval = this.cycleInterval | 0;
              nextIrq = interval > 0 ? (Math.floor(cycle / interval) + 1) * interval : 0;
              chk = this.needChecks(); nextChk = 0; lim = -1;
            }
            break;
          case 12: /* SC  */
            x = i32[sp >> 2]; sp += 4;
            if (x >= 0x1000) u8[x] = a;
            else {
              this.pc = pc; this.sp = sp; this.bp = bp; this.a = a; this.cycle = cycle;
              this.st8(x, a);
              if (dev.halted) { stop = STOP.HALT; break loop; }
              interval = this.cycleInterval | 0;
              nextIrq = interval > 0 ? (Math.floor(cycle / interval) + 1) * interval : 0;
              chk = this.needChecks(); nextChk = 0; lim = -1;
            }
            a = (a << 24) >> 24;
            break;
          case 13: /* PSH */ sp -= 4; i32[sp >> 2] = a; break;
          case 14: /* OR  */ a = i32[sp >> 2] | a; sp += 4; break;
          case 15: /* XOR */ a = i32[sp >> 2] ^ a; sp += 4; break;
          case 16: /* AND */ a = i32[sp >> 2] & a; sp += 4; break;
          case 17: /* EQ  */ a = i32[sp >> 2] === a ? 1 : 0; sp += 4; break;
          case 18: /* NE  */ a = i32[sp >> 2] !== a ? 1 : 0; sp += 4; break;
          case 19: /* LT  */ a = i32[sp >> 2] < a ? 1 : 0; sp += 4; break;
          case 20: /* GT  */ a = i32[sp >> 2] > a ? 1 : 0; sp += 4; break;
          case 21: /* LE  */ a = i32[sp >> 2] <= a ? 1 : 0; sp += 4; break;
          case 22: /* GE  */ a = i32[sp >> 2] >= a ? 1 : 0; sp += 4; break;
          case 23: /* SHL */ a = i32[sp >> 2] << a; sp += 4; break;
          case 24: /* SHR */ a = i32[sp >> 2] >> a; sp += 4; break;
          case 25: /* ADD */ a = (i32[sp >> 2] + a) | 0; sp += 4; break;
          case 26: /* SUB */ a = (i32[sp >> 2] - a) | 0; sp += 4; break;
          case 27: /* MUL */ a = Math.imul(i32[sp >> 2], a); sp += 4; break;
          case 28: /* DIV */ a = a === 0 ? 0 : (i32[sp >> 2] / a) | 0; sp += 4; break;
          case 29: /* MOD */ a = a === 0 ? 0 : (i32[sp >> 2] % a) | 0; sp += 4; break;

          // The fused opcodes, with c4mp's meanings (src/c4mp/vm.c:395-404).
          // c4m has only LDL, STL and POPA; the other seven are here so the
          // disk's -mfuse images run, as they do on c4bb.
          case 79: /* LDL  */ a = i32[(bp >> 2) + i32[pc >> 2]]; pc += 4; break;
          case 80: /* LDG  */ x = i32[pc >> 2]; pc += 4; a = ((x & 3) === 0 && x >= 0x1000) ? i32[x >> 2] | 0 : (this.cycle = cycle, this.ld32(x)); break;
          case 81: /* PSHL */ a = i32[(bp >> 2) + i32[pc >> 2]]; pc += 4; sp -= 4; i32[sp >> 2] = a; break;
          case 82: /* PSHG */ x = i32[pc >> 2]; pc += 4; a = ((x & 3) === 0 && x >= 0x1000) ? i32[x >> 2] | 0 : (this.cycle = cycle, this.ld32(x)); sp -= 4; i32[sp >> 2] = a; break;
          case 83: /* LEAP */ a = (bp + (i32[pc >> 2] << 2)) | 0; pc += 4; sp -= 4; i32[sp >> 2] = a; break;
          case 84: /* IMMP */ a = i32[pc >> 2]; pc += 4; sp -= 4; i32[sp >> 2] = a; break;
          case 85: /* LIP  */ a = ((a & 3) === 0 && a >= 0x1000) ? i32[a >> 2] | 0 : (this.cycle = cycle, this.ld32(a)); sp -= 4; i32[sp >> 2] = a; break;
          case 86: /* ADDL */ x = (i32[sp >> 2] + a) | 0; sp += 4; a = ((x & 3) === 0 && x >= 0x1000) ? i32[x >> 2] | 0 : (this.cycle = cycle, this.ld32(x)); break;
          case 87: /* STL  */ i32[(bp >> 2) + i32[pc >> 2]] = a; pc += 4; break;
          case 88: /* POPA */ a = i32[sp >> 2]; sp += 4; break;

          case 61: /* JSRI */ x = i32[pc >> 2]; sp -= 4; i32[sp >> 2] = pc + 4; pc = this.ld32(x); break;
          case 62: /* JSRS */ x = i32[pc >> 2]; sp -= 4; i32[sp >> 2] = pc + 4; pc = i32[(bp >> 2) + x]; break;
          case 63: /* JMPA */ pc = a; break;
          case 49: /* _JMP */ pc = i32[sp >> 2]; sp += 4; break;
          case 50: /* _ADJ */ sp = (sp + (i32[sp >> 2] << 2)) | 0; break;
          case 52: /* C4CY */ a = cycle | 0; break;

          case 48: /* OPCD */
            // Run the opcode on top of the stack. The ones that carry an
            // operand would read it from the caller's code, so they are
            // refused with a (usually missed) OPV and a harmless C4CY.
            // A syscall or a custom opcode goes on to default, below,
            // and one of this switch's own to opcd(). Never round the
            // switch again: a loop about it makes V8 compile every case
            // worse, and C4IX's syscalls are all OPCDs. Its programs ran
            // 1.7 times slower for it.
            op = i32[sp >> 2];
            if (hasOperand(op)) {
              const nm = i => OPNAMES.slice(i * 5, i * 5 + 4);
              this.pc = pc; this.sp = sp; this.bp = bp; this.a = a; this.cycle = cycle;
              this.emitString(`${nm(OP.OPCD)} does not support opcodes requiring arguments (${nm(op)} given)\n`);
              this.trap(TRAP.OPV, op, this.trapHandler);
              this.cycleInterval = 0; this.mode = 0;
              pc = this.pc; sp = this.sp; bp = this.bp; a = this.a;
              interval = 0; nextIrq = 0;
              chk = this.needChecks(); nextChk = 0; lim = -1;
              a = cycle | 0;                        // C4CY
              break;
            }
            if (isPlainOp(op)) {
              this.pc = pc; this.sp = sp; this.bp = bp; this.a = a; this.cycle = cycle;
              this.opcd(op);
              pc = this.pc; sp = this.sp; bp = this.bp; a = this.a;
              if (dev.halted) { stop = STOP.HALT; break loop; }
              interval = this.cycleInterval | 0;
              nextIrq = interval > 0 ? (Math.floor(cycle / interval) + 1) * interval : 0;
              chk = this.needChecks(); nextChk = 0; lim = -1;
              break;
            }
            // falls through

          default: {
            // Everything else: syscalls, traps, TLEV, custom opcodes.
            this.pc = pc; this.sp = sp; this.bp = bp; this.a = a; this.cycle = cycle;
            const s = this.sys(op);
            pc = this.pc; sp = this.sp; bp = this.bp; a = this.a;
            cycle = this.cycle;
            interval = this.cycleInterval | 0;
            nextIrq = interval > 0 ? (Math.floor(cycle / interval) + 1) * interval : 0;
            chk = this.needChecks(); nextChk = 0; lim = -1;
            if (s !== STOP.BUDGET) { stop = s; break loop; }
            if (dev.halted) { stop = STOP.HALT; break loop; }
          }
        }
      }
    }

    this.pc = pc; this.sp = sp; this.bp = bp; this.a = a; this.cycle = cycle;
    if (dev.halted) stop = STOP.HALT;
    return stop;
  }
}
Machine.f32 = new Float32Array(1);
Machine.fi32 = new Int32Array(Machine.f32.buffer);

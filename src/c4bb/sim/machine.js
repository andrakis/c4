// machine.js - the c4bb CPU: registers, microstep engine, dispatch.
//
// Executes the tables assembled from hw/microcode.uc one microstep at
// a time, emitting an event per step when a listener is attached (the
// board renderer replays these). Reference semantics: the c4m.c VM
// loop at c4m.c:1490.
//
// The "system controller" (jsops) covers the opcodes that will become
// real microcode when the trap machinery lands in M2; each executes as
// a single visible SYSCTL microstep, mirroring c4m.c's behavior
// exactly (including missed-trap silence, the oisc4 convention).

import { FETCH, DISPATCH, ALU_OPS } from './ucode.js';
import { OPNAMES } from './devices.js';

export const R = { PC: 0, SP: 1, BP: 2, A: 3, IR: 4, OPR: 5, MAR: 6, MDR: 7, B: 8, T: 9, U: 10 };
export const REG_NAMES = ['PC', 'SP', 'BP', 'A', 'IR', 'OPR', 'MAR', 'MDR', 'B', 'T', 'U'];

// Opcode numbers (c4m.c:261)
export const OP = {};
OPNAMES.match(/.{5}/g).forEach((n, i) => { OP[n.slice(0, 4).trim()] = i; });
// 89, not 66: the opcode ROM now names c4mp's 66-78 and the fused
// 79-88 as well. Naming is not implementing -- an opcode with no
// microcode has dispatchTab[-1] and misses its trap exactly as an
// unknown one did before. What changes is that the board can EXECUTE
// LDL/STL/POPA and NAME the rest (docs/fused-opcodes.md).
export const INS_SIZE = 89;

// The single place that decides which opcodes carry an operand word,
// mirroring c4m_has_operand (c4m.c:344) exactly -- including JSRI and
// JSRS, which this board's OPCD guard used to miss.
export function hasOperand (ir) {
  return ir <= OP.ADJ || ir === OP.JSRI || ir === OP.JSRS
      || (ir >= OP.LDL && ir <= OP.IMMP)   // LIP, ADDL and POPA take none
      || ir === OP.STL;
}

const TRAP_NAMES = ['TRAP_ILLOP', 'TRAP_HARD_IRQ', 'TRAP_SOFT_IRQ', 'TRAP_SIGNAL',
                    'TRAP_SEGV', 'TRAP_OPV', 'TRAP_PM_VIOLATION', 'TRAP_DEBUG'];

// Opcodes that trap with TRAP_PM_VIOLATION in protected mode
// (the guarded cases in c4m.c's dispatch: OPEN READ CLOS PRTF MALC
// FREE PUTC PUTS RALC EXIT INFO; MSET/MCMP/MCPY/STRC/ITH/C4CF are not
// gated there, deliberately mirrored).
//
// RALC (41) was missing here while c4m.c:1922 has always gated it. It
// mattered the moment C4KE started tracking a protected task's
// allocations (docs/task-memory.md): a realloc that traps on one host
// and does not on the other is an allocation the kernel knows about on
// one host and not the other.
export const PM_GATED = new Set([30, 31, 32, 33, 34, 35, 38, 39, 40, 41, 57]);

export class Machine {
  constructor(arena, ucode, devices, opts = {}) {
    this.arena = arena;
    this.ucode = ucode;
    this.dev = devices;
    devices.machine = this;
    arena.io = devices;
    this.regs = new Int32Array(16);
    this.cycle = 0;
    this.upc = FETCH;
    this.mode = 0;                    // MODE_UNPROTECTED
    this.trapHandler = 0;             // ITH latch
    this.cycleInterval = 0;           // C4CF latches
    this.cycleHandler = 0;
    this.trapRestoresInterval = 0;
    this.signalHandlers = new Map();
    this.pendingSignal = 0;
    this.tt = 0; this.tp = 0; this.hnd = 0;   // trap jam latches
    this.onEvent = null;              // fn(event) for the renderer
    this.onLog = opts.onLog || (s => process.stderr.write(s));
    this.uPerOp = null;               // filled lazily for stats

    // dispatch table: opcode -> step index (or -1 = unknown)
    this.dispatchTab = new Int32Array(INS_SIZE + 1).fill(-1);
    for (const [name, entry] of ucode.ops) {
      const num = OP[name];
      if (num === undefined) throw new Error(`microcode op ${name} has no opcode number`);
      this.dispatchTab[num] = entry.start;
    }
    this.fetchStart = ucode.routines.get('fetch');
    this.trapStart = ucode.routines.get('trap');
  }

  // The trap jam: latch type/parameter/handler and redirect the
  // sequencer into the trap microroutine. Site effects reproduce what
  // each c4m call site does around trap() - the sites differ (see
  // c4m.c: HIRQ/OPV/PM zero the interval and drop protection, ILLOP
  // only drops protection, _TRP/DBG touch nothing). A missed trap
  // (no handler) still applies the site effects, exactly like c4m's
  // early return inside trap() before the interval is zeroed.
  // Returns the next microstep index.
  jamTrap(type, param, handler, { zeroInterval = false, unprot = false } = {}) {
    // snapshot BEFORE site effects: the trap frame carries the mode
    // and interval as they were when the trap was raised
    this.jmode = this.mode;
    this.jinterval = this.cycleInterval;
    if (zeroInterval) this.cycleInterval = 0;
    if (unprot) this.mode = 0;
    if (!handler) {
      this.missedTrap(type, param);
      return FETCH;
    }
    this.tt = type | 0;
    this.tp = param | 0;
    this.hnd = handler | 0;
    return this.trapStart;
  }

  // Instruction-boundary interrupt checks (c4m.c:1501-1531). Returns
  // the microstep to start this instruction with: the fetch routine,
  // or the trap routine when an interrupt fires. The cycle counter
  // has already been incremented.
  //
  // Cycle accounting: in c4m a boundary trap and the handler's FIRST
  // instruction share one loop iteration (trap() returns and i=*pc++
  // fetches at handler+2 in the same pass), while a dispatch-time
  // trap (ILLOP/OPV/PM) ends its iteration and the handler starts on
  // a fresh ++cycle. The trap microroutine always ends at the
  // instruction boundary, so boundary jams give back their increment
  // here to keep the two cases distinct and cycle-exact.
  boundaryChecks() {
    if (this.cycleInterval && this.cycle % this.cycleInterval === 0 &&
        this.arena.read32(this.regs[R.PC]) !== OP.TLEV) {
      // TLEV is uninterruptible: it restores five registers atomically
      const t = this.jamTrap(1 /* HARD_IRQ */, 0 /* HIRQ_CYCLE */, this.cycleHandler,
                             { zeroInterval: true, unprot: true });
      if (t !== FETCH) { this.cycle--; return t; }
    } else if (this.pendingSignal &&
               !(this.trapRestoresInterval && !this.cycleInterval)) {
      const sig = this.pendingSignal;
      const t = this.jamTrap(3 /* SIGNAL */, sig, this.signalHandlers.get(sig) | 0,
                             { zeroInterval: true, unprot: true });
      this.pendingSignal = 0;
      if (t !== FETCH) { this.cycle--; return t; }
    }
    return this.fetchStart;
  }

  flag(name) {
    if (name === 'az') return this.regs[R.A] === 0;
    if (name === 'tz') return this.regs[R.T] === 0;
    if (name === 'mz') return this.regs[R.MDR] === 0;
    throw new Error(`bad flag ${name}`);
  }

  aluCompute(step) {
    const l = step.aluLA ? this.regs[R.A] : this.regs[R.B];
    const r = step.aluR === 'T' ? this.regs[R.T]
            : step.aluR === 'MDR' ? this.regs[R.MDR]
            : this.regs[R.A];
    switch (ALU_OPS[step.aluOp]) {
      case 'OR': return (l | r) | 0;
      case 'XOR': return (l ^ r) | 0;
      case 'AND': return (l & r) | 0;
      case 'EQ': return l === r ? 1 : 0;
      case 'NE': return l !== r ? 1 : 0;
      case 'LT': return l < r ? 1 : 0;
      case 'GT': return l > r ? 1 : 0;
      case 'LE': return l <= r ? 1 : 0;
      case 'GE': return l >= r ? 1 : 0;
      case 'SHL': return (l << (r & 31)) | 0;
      case 'SHR': return (l >> (r & 31)) | 0;
      case 'ADD': return (l + r) | 0;
      case 'SUB': return (l - r) | 0;
      case 'MUL': return Math.imul(l, r);
      case 'DIV': return r === 0 ? 0 : (l / r) | 0;
      case 'MOD': return r === 0 ? 0 : (l % r) | 0;
    }
  }

  // Execute one microstep. Returns false when halted.
  step() {
    if (this.dev.halted) return false;
    const regs = this.regs, arena = this.arena;

    if (this.upc === FETCH) {
      // instruction boundary: c4m.c:1491 ++cycle, then interrupt checks
      this.cycle++;
      this.upc = this.boundaryChecks();
    }

    const step = this.ucode.steps[this.upc];

    if (step.jsop) {
      const jam = this.execJsop(step.jsop);
      if (this.onEvent) this.onEvent({ upc: this.upc, step, bus: 0, jsop: step.jsop });
      this.upc = jam !== undefined ? jam : (step.next === undefined ? FETCH : step.next);
      return !this.dev.halted;
    }

    // 1. memory read into MDR
    if (step.mem === 'MEM_RD') regs[R.MDR] = arena.read32(regs[R.MAR]);
    else if (step.mem === 'MEM_RDB') regs[R.MDR] = arena.read8s(regs[R.MAR]);
    else if (step.mem === 'MEM_RDBU') regs[R.MDR] = arena.read8u(regs[R.MAR]);

    // 2. bus
    let bus = 0;
    const d = step.drivers[0];
    if (d) {
      switch (d) {
        case 'PC_OUT': bus = regs[R.PC]; break;
        case 'SP_OUT': bus = regs[R.SP]; break;
        case 'BP_OUT': bus = regs[R.BP]; break;
        case 'A_OUT': bus = regs[R.A]; break;
        case 'OPR_OUT': bus = regs[R.OPR]; break;
        case 'OPR_OUTX4': bus = (regs[R.OPR] << 2) | 0; break;
        case 'MDR_OUT': bus = regs[R.MDR]; break;
        case 'B_OUT': bus = regs[R.B]; break;
        case 'T_OUT': bus = regs[R.T]; break;
        case 'T_OUTX4': bus = (regs[R.T] << 2) | 0; break;
        case 'U_OUT': bus = regs[R.U]; break;
        case 'CYC_OUT': bus = this.cycle | 0; break;
        case 'ALU_OUT': bus = this.aluCompute(step); break;
      }
    } else if (step.cval !== null) bus = step.cval;

    // 3. latches
    for (const l of step.latches) {
      switch (l) {
        case 'PC_IN': regs[R.PC] = bus; break;
        case 'SP_IN': regs[R.SP] = bus; break;
        case 'BP_IN': regs[R.BP] = bus; break;
        case 'A_IN': regs[R.A] = bus; break;
        case 'IR_IN': regs[R.IR] = bus; break;
        case 'OPR_IN': regs[R.OPR] = bus; break;
        case 'MAR_IN': regs[R.MAR] = (bus + (step.marOfs << 2)) | 0; break;
        case 'MDR_IN': regs[R.MDR] = bus; break;
        case 'B_IN': regs[R.B] = bus; break;
        case 'T_IN': regs[R.T] = bus; break;
        case 'U_IN': regs[R.U] = bus; break;
      }
    }

    // 4. memory write from MDR
    if (step.memWrite === 'MEM_WR') arena.write32(regs[R.MAR], regs[R.MDR]);
    else if (step.memWrite === 'MEM_WRB') arena.write8(regs[R.MAR], regs[R.MDR] & 0xff);

    // 5. counters
    for (const c of step.counters) {
      switch (c) {
        case 'PC_INC': regs[R.PC] = (regs[R.PC] + 4) | 0; break;
        case 'SP_INC': regs[R.SP] = (regs[R.SP] + 4) | 0; break;
        case 'SP_DEC': regs[R.SP] = (regs[R.SP] - 4) | 0; break;
        case 'T_DEC1': regs[R.T] = (regs[R.T] - 1) | 0; break;
        case 'U_INC1': regs[R.U] = (regs[R.U] + 1) | 0; break;
        case 'B_INC1': regs[R.B] = (regs[R.B] + 1) | 0; break;
      }
    }

    if (this.onEvent) this.onEvent({ upc: this.upc, step, bus });

    // 6. next microstep
    let next;
    if (step.brFlag !== null && (this.flag(step.brFlag) !== step.brNeg)) {
      next = step.brTarget;
    } else {
      next = step.next;
    }
    if (next === DISPATCH) {
      this.upc = this.decode(regs[R.IR]);
    } else {
      this.upc = next;
    }
    return !this.dev.halted;
  }

  decode(ir) {
    // OPCD guard, c4m.c:1535: operand-carrying opcodes cannot be
    // re-dispatched; complain, raise (missed) OPV, execute C4CY.
    // Detect "came from OPCD" by the current step's scope.
    const step = this.ucode.steps[this.upc];
    if (step.scope === 'OPCD' && hasOperand(ir)) {
      const name = i => OPNAMES.slice(i * 5, i * 5 + 4);
      this.printVm(`${name(OP.OPCD)} does not support opcodes requiring arguments (${name(ir)} given)\n`);
      const t = this.jamTrap(5 /* TRAP_OPV */, ir, this.trapHandler,
                             { zeroInterval: true, unprot: true });
      if (t !== FETCH) return t;
      ir = OP.C4CY;   // missed: c4m substitutes a harmless C4CY
    }
    if (ir >= 0 && ir < INS_SIZE) {
      // Protected-mode gate: syscall opcodes trap instead of running
      // (each c4m case checks mode before acting; the decoder is the
      // equivalent single place). EXIT is gated too (c4m.c:1713).
      if (this.mode === 1 && PM_GATED.has(ir)) {
        return this.jamTrap(6 /* PM_VIOLATION */, ir, this.trapHandler,
                            { zeroInterval: true, unprot: true });
      }
      const t = this.dispatchTab[ir];
      if (t >= 0) return t;
    }
    // Unknown or unimplemented opcode: TRAP_ILLOP (c4m.c:1858). The
    // call site only drops protection; the interval is untouched.
    return this.jamTrap(0 /* TRAP_ILLOP */, ir, this.trapHandler, { unprot: true });
  }

  missedTrap(type, ins) {
    // c4m prints trap chatter here (filtered out in diff tests);
    // like oisc4 we stay silent but count it for the renderer.
    this.missedTraps = (this.missedTraps || 0) + 1;
    this.lastTrap = { type, name: TRAP_NAMES[type], ins, cycle: this.cycle };
  }

  // VM-level output (not program output): goes to the same stream so
  // ordering matches native c4m, which prints via the same stdout.
  printVm(s) {
    for (const b of new TextEncoder().encode(s)) this.dev.write32(0x100, b);
  }

  execJsop(name) {
    const regs = this.regs, arena = this.arena;
    const sp = regs[R.SP];
    const sp0 = () => arena.read32(sp), sp1 = () => arena.read32(sp + 4);
    switch (name) {
      case 'ITH': {                          // c4m.c:1764
        const old = this.trapHandler;
        this.trapHandler = sp0();
        regs[R.A] = old | 0;
        break;
      }
      case 'C4CF': {                         // c4m.c:1782
        const opt = sp1(), val = sp0();
        if (opt === 0) { regs[R.A] = this.cycleInterval; this.cycleInterval = val; }
        else if (opt === 1) { regs[R.A] = this.cycleHandler; this.cycleHandler = val; }
        else if (opt === 3) { regs[R.A] = this.trapRestoresInterval; this.trapRestoresInterval = val; }
        else { this.dev.halted = true; this.dev.status = -100; }  // CONF_PRIVS etc
        break;
      }
      case 'SIGH': {                         // include/c4.h:85
        const sig = sp1(), handler = sp0();
        const old = this.signalHandlers.get(sig) || 0;
        this.signalHandlers.set(sig, handler);
        regs[R.A] = old | 0;
        break;
      }
      case 'SIGI': regs[R.A] = 2; break;     // SIGINT
      case '_TRP':                           // c4m.c:1820: __c4_trap(type, param)
        // trap(sp[1], sp[0], ...); the site touches neither interval
        // nor mode (both lines commented out at c4m.c:1825)
        return this.jamTrap(sp1(), sp0(), this.trapHandler, {});
      case 'DBG':                            // c4m.c:1827
        return this.jamTrap(7 /* TRAP_DEBUG */, 0, this.trapHandler, {});
      case 'C4IV': break;                    // native no-op (c4m.c:1832)
      case 'FLT': regs[R.A] = this.fltInstruction(sp); break;
    }
    return undefined;
  }

  // Port of c4_float_instruction (c4m_float.c:31): binary32 in a word.
  fltInstruction(sp) {
    const f = new Float32Array(1), fi = new Int32Array(f.buffer);
    const bits = v => { f[0] = v; return fi[0]; };
    const flt = i => { fi[0] = i; return f[0]; };
    const op = this.arena.read32(sp);
    const a = flt(this.arena.read32(sp + 4));
    const b = flt(this.arena.read32(sp + 8));
    let c;
    switch (op) {
      case 0: c = a < 0 ? -a : a; break;             // ABSF
      case 1: c = a + b; break;
      case 2: c = a - b; break;
      case 3: c = a * b; break;
      case 4: c = a / b; break;
      case 5: c = -a; break;
      case 6: c = Math.sin(a); break;
      case 7: c = Math.cos(a); break;
      case 8: c = Math.tan(a); break;
      case 9: c = this.arena.read32(sp + 4); break;  // ITOF: int -> float
      case 10: c = Math.trunc(a); break;             // FTOI: c4m stores the
                                                     // truncated value back in
                                                     // a float and returns its
                                                     // BITS (c4m_float.c:52)
      default:
        this.printVm(`c4m fatal error: unsupported floating point instruction ${op}!\n`);
        c = 0;
    }
    return bits(Math.fround(c));
  }

  // Run whole instructions until halt or budget exhausted.
  run(maxCycles = Infinity) {
    const limit = this.cycle + maxCycles;
    while (!this.dev.halted && this.cycle < limit) {
      if (!this.step()) break;
    }
    return !this.dev.halted;
  }

  // Run until PC equals target at an instruction boundary (loader use).
  runUntilPc(target, maxCycles = 1e9) {
    const limit = this.cycle + maxCycles;
    while (!this.dev.halted && this.cycle < limit) {
      if (this.upc === FETCH && this.regs[R.PC] === target) return true;
      if (!this.step()) return false;
    }
    return false;
  }
}

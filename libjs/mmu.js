// mmu.js - paged virtual memory with protection, for C4IX (docs/c4ix-mmu.md).
//
// A Machine (c4m.js) gets an Mmu when the host fits one (createMachine's
// mmu option). It does nothing until the guest turns it on through its
// registers, and then only in user mode: kernel mode (mode 0) runs the
// interpreter's own loop, untranslated, exactly as a machine without an
// MMU does, and user mode (mode 1) runs runUser below, which translates
// every fetch, load and store through the guest's page tables.
//
// Page tables are the guest's, in its memory: two levels of 1,024 words,
// the i386's shape (docs/c4ix-mmu.md section 4). Translations are cached
// per CPU in three arrays indexed by virtual page -- read, write, fetch --
// holding the physical page or -1, and two windows sit in front of them:
// the run of stack pages around sp and the code page around pc, so that
// the accesses c4 code makes most compare against locals instead.
//
// A fault is precise: the registers the trap saves are the faulting
// instruction's, before it changed any, so the instruction can run again.

import { TLEV_ADDR } from '../src/c4bb/sim/arena.js';
import { OP, STOP, TRAP, CPU_HALT_ADDR, LOCAL_BASE, LOCAL_END, C4I_MMU, MMU_BASE, MMU_END,
         hasOperand, isRom, isPlainOp } from './c4m.js';

export { C4I_MMU, MMU_BASE, MMU_END };
export const MMU = {
  CTL: 0x500, PTBR: 0x504, KSP: 0x508, FADDR: 0x50c, FINFO: 0x510, FPC: 0x514,
  INVLPG: 0x518, FLUSH: 0x51c, IOPM: 0x520, MISSES: 0x524, FAULTS: 0x528, MEMSIZE: 0x52c,
  UWIN: 0x530, UDIR: 0x534,
};
export const PTE = { P: 1, W: 2, U: 4, X: 8, A: 0x20, D: 0x40 };
// What a fault was (FINFO): the access, and whether the page was there.
// DMA: a device's, at an address the task gave it (dmaCheck).
export const FK = { READ: 1, WRITE: 2, FETCH: 4, PRESENT: 8, DMA: 32 };
// Page 0's windows a user task may use (IOPM, section 7). At reset it may
// use all of them, as without an MMU; a kernel takes away what it keeps.
export const IOPM = { ROM: 1, DEV: 2, GUI: 4, SND: 8, LOCAL: 16, LOCALW: 32, ALL: 63 };

// Virtual pages the arrays cache: 512 MB, the user window (UWIN, at 256
// MB) and memory below it. Above it every access walks.
const LIMIT = 131072;
// Pages the stack window grows by, each way, when it is made, and the code
// window: a program's whole text, so that its calls stay inside it
const STACK_GROW = 16, CODE_GROW = 256;
// Code windows kept, so that a program and its libraries' code, which are
// apart, are not made again each time a call goes from one to the other
const CODE_WINS = 4;

// Thrown by the translated ld32/st32 that sys() and the FPU call in user
// mode; the user loop catches it and raises the fault
export const FAULT = { fault: true };

// The page table entry for va in the space at ptbr, from the tables in
// i32 (an arena of size bytes): 0 when there is none
function pteOf(i32, size, ptbr, va) {
  if (!ptbr) return 0;
  const pde = i32[(ptbr >>> 2) + (va >>> 22)] | 0;
  if (!(pde & 1)) return 0;
  const pt = pde & -4096;
  if ((pt >>> 0) + 4096 > size) return 0;
  return i32[(pt >>> 2) + ((va >>> 12) & 1023)] | 0;
}

// Where [addr, addr + len) is in memory, for a device, in the space at
// ptbr: every page of it user mode's to read (and write, if write), one
// run of memory, and the physical address of addr returned. Else -1, and
// dmaBad says where it went wrong and dmaBadPte what the entry there was.
let dmaBad = 0, dmaBadPte = 0;
export function dmaPhys(i32, size, ptbr, addr, len, write) {
  addr = addr >>> 0;
  if (len < 1) len = 1;
  const end = addr + len;
  if (addr < 0x1000 || end > 0x100000000) { dmaBad = addr; dmaBadPte = 0; return -1; }
  const need = PTE.P | PTE.U | (write ? PTE.W : 0);
  let pa = -1, next = 0;
  for (let va = addr & -4096; va < end; va += 4096) {
    const pte = pteOf(i32, size, ptbr, va), f = pte & -4096;
    if ((pte & need) !== need || (f >>> 0) + 4096 > size || (pa >= 0 && f !== next)) {
      dmaBad = va < addr ? addr : va; dmaBadPte = pte; return -1;
    }
    if (pa < 0) pa = f | (addr & 4095);
    next = f + 4096;
  }
  return pa;
}
const dmaKind = write => (write ? FK.WRITE : FK.READ) | FK.DMA | (dmaBadPte & PTE.P ? FK.PRESENT : 0);

// What a device sees of a user task's space when another CPU's task is
// the one at it (par.js serves those on CPU 0): the asker's PTBR. A
// refusal is kept for the asker to raise.
export class DmaSpace {
  constructor(arena) { this.arena = arena; this.ptbr = 0; this.faddr = -1; this.finfo = 0; }
  dmaCheck(addr, len, write) {
    const a = this.arena;
    const pa = dmaPhys(a.i32, a.size, this.ptbr, addr, len, write);
    if (pa >= 0) return pa;
    this.faddr = dmaBad; this.finfo = dmaKind(write);
    throw FAULT;
  }
}

export class Mmu {
  constructor(m) {
    this.m = m;
    this.rd = new Int32Array(LIMIT).fill(-1);
    this.wr = new Int32Array(LIMIT).fill(-1);
    this.ex = new Int32Array(LIMIT).fill(-1);
    this.nx = new Uint8Array(LIMIT);       // 1: known not to be runnable (xPage), until a flush
    this.cwins = new Int32Array(3 * CODE_WINS);   // lo, len, off of each code window kept
    this.log = new Int32Array(LIMIT);
    this.nlog = 0;
    this.misses = 0;
    this.faults = 0;
    this.reset();
  }

  // Off, as at power-on and when a CPU is started (smp.js start)
  reset() {
    this.on = false;
    this.m.uwin = 0; this.m.udir = 0;
    this.general = false;          // a window has been other than identity: runUser from now on
    this.ptbr = 0;
    this.ksp = 0;
    this.iopm = IOPM.ALL;
    this.faddr = 0; this.finfo = 0; this.fpc = 0;
    this.ncwins = 0;
    this.flush();
  }

  flush() {
    const rd = this.rd, wr = this.wr, ex = this.ex, nx = this.nx, log = this.log;
    for (let k = 0; k < this.nlog; k++) { const v = log[k]; rd[v] = -1; wr[v] = -1; ex[v] = -1; nx[v] = 0; }
    this.nlog = 0;
    this.ncwins = 0;
    this.sLo = 0; this.sLen = 0; this.sOff = 0;
    this.cLo = 0; this.cLen = 0; this.cOff = 0;
  }

  invlpg(va) {
    const v = va >>> 12;
    if (v < LIMIT) { this.rd[v] = -1; this.wr[v] = -1; this.ex[v] = -1; this.nx[v] = 0; }
    this.ncwins = 0;
    this.sLo = 0; this.sLen = 0; this.sOff = 0;
    this.cLo = 0; this.cLen = 0; this.cOff = 0;
  }

  // ---------------------------------------------------------------
  // The registers, at 0x500 on every CPU (each its own: c4m.js routes
  // the window here before the bus). Kernel mode only: user mode's
  // accesses below 0x1000 come through low() and never reach them.
  read32(x) {
    switch (x & ~3) {
      case MMU.CTL: return this.on ? 3 : 1;
      case MMU.PTBR: return this.ptbr;
      case MMU.KSP: return this.ksp;
      case MMU.FADDR: return this.faddr;
      case MMU.FINFO: return this.finfo;
      case MMU.FPC: return this.fpc;
      case MMU.IOPM: return this.iopm;
      case MMU.MISSES: { const n = this.misses; this.misses = 0; return n; }
      case MMU.FAULTS: return this.faults;
      case MMU.MEMSIZE: return this.m.arena.size;
      case MMU.UWIN: return this.m.uwin;
      case MMU.UDIR: return this.m.udir;
    }
    return 0;
  }
  write32(x, v) {
    switch (x & ~3) {
      case MMU.CTL: this.on = (v & 1) !== 0; this.flush(); return;
      case MMU.PTBR: this.ptbr = v & ~4095; this.flush(); return;
      case MMU.KSP: this.ksp = v & ~3; return;
      case MMU.INVLPG: this.invlpg(v); return;
      case MMU.FLUSH: this.flush(); return;
      case MMU.IOPM: this.iopm = v; return;
      // the user window: where user addresses start (c4m.js kpa). The
      // arena's devices reach it as the kernel does.
      case MMU.UWIN: this.m.uwin = v & -4096; this.m.arena.uwin = this.m.uwin; return;
      case MMU.UDIR: this.m.udir = v & -4096; return;
    }
  }

  // ---------------------------------------------------------------
  // Translation. The page table entry for va, without side effects:
  // 0 when there is none. pteAt is the word it was read from (-1).
  probe(va) {
    const i32 = this.m.i32, size = this.m.arena.size, pd = this.ptbr;
    this.pteAt = -1;
    if (!pd) return 0;
    const pde = i32[(pd >>> 2) + (va >>> 22)] | 0;
    if (!(pde & 1)) return 0;
    const pt = pde & -4096;
    if ((pt >>> 0) + 4096 > size) return 0;
    this.pteAt = (pt >>> 2) + ((va >>> 12) & 1023);
    return i32[this.pteAt] | 0;
  }

  // The physical page for an access of this kind (FK.READ, WRITE, FETCH)
  // at va, filling the arrays; or -1, with the fault recorded (FADDR,
  // FINFO, FPC) for raise(). va is at or above 0x1000.
  xlate(va, kind) {
    const pte = this.probe(va);
    const frame = pte & -4096;
    if ((pte & 5) !== 5 || (frame >>> 0) + 4096 > this.m.arena.size ||
        (kind === FK.WRITE && !(pte & PTE.W)) || (kind === FK.FETCH && !(pte & PTE.X))) {
      this.faddr = va | 0;
      this.finfo = kind | ((pte & PTE.P) ? FK.PRESENT : 0);
      this.fpc = this.m.pc;
      this.faults++;
      return -1;
    }
    let npte = pte | PTE.A;
    if (kind === FK.WRITE) npte |= PTE.D;
    if (npte !== pte) this.m.i32[this.pteAt] = npte;
    this.misses++;
    const v = va >>> 12;
    if (v < LIMIT) {
      if (this.rd[v] < 0 && this.wr[v] < 0 && this.ex[v] < 0 && !this.nx[v]) this.logv(v);
      this.rd[v] = frame;
      // written to only once it is dirty, so the first store sets D
      if ((npte & (PTE.W | PTE.D)) === (PTE.W | PTE.D)) this.wr[v] = frame;
      if (npte & PTE.X) this.ex[v] = frame;
    }
    return frame;
  }

  // V's entries are about to be set: logged, so that a flush clears them
  logv(v) {
    if (this.nlog === LIMIT) {
      this.rd.fill(-1); this.wr.fill(-1); this.ex.fill(-1); this.nx.fill(0);
      this.nlog = 0;
    }
    this.log[this.nlog++] = v;
  }

  // The physical page at va if user mode may run it, filled; -1 if not,
  // which is remembered until the next flush. Raises nothing.
  xPage(va) {
    if ((va >>> 0) < 0x1000) return -1;
    const v = va >>> 12;
    if (v < LIMIT) {
      if (this.ex[v] >= 0) return this.ex[v];
      if (this.nx[v]) return -1;
    }
    const pte = this.probe(va);
    if ((pte & (PTE.P | PTE.U | PTE.X)) !== (PTE.P | PTE.U | PTE.X)) {
      if (v < LIMIT) {
        if (this.rd[v] < 0 && this.wr[v] < 0) this.logv(v);
        this.nx[v] = 1;
      }
      return -1;
    }
    return this.xlate(va, FK.FETCH);
  }

  // The cached page, or a fill: the slow path's first step
  page(va, kind) {
    const v = va >>> 12;
    if (v < LIMIT) {
      const p = kind === FK.READ ? this.rd[v] : kind === FK.WRITE ? this.wr[v] : this.ex[v];
      if (p >= 0) return p;
    }
    return this.xlate(va, kind);
  }

  // Raise the recorded fault as TRAP_SEGV(FADDR). The machine's registers
  // are the faulting instruction's (the callers saved them). With no
  // handler there is nobody to give it to: the machine stops, saying why.
  raise() {
    const m = this.m;
    if (!m.trapHandler) {
      const hex = n => '0x' + ((n >>> 0).toString(16).toUpperCase().padStart(8, '0'));
      const what = this.finfo & FK.WRITE ? 'write to' : this.finfo & FK.FETCH ? 'fetch from' : 'read of';
      m.emitString(`libjs: page fault: ${what} ${hex(this.faddr)} at pc ${hex(this.fpc)}, and no trap handler. Halting.\n`);
      m.vm = false;
      m.halt(-104);
      return;
    }
    m.trap(TRAP.SEGV, this.faddr, m.trapHandler);
  }

  // A fault found outside xlate: a user access to page 0 the IOPM refuses,
  // or a fetch there
  refuse(va, kind) {
    this.faddr = va | 0;
    this.finfo = kind;
    this.fpc = this.m.pc;
    this.faults++;
  }

  // A device, reached by this task's load or store, about to read (or
  // write) [addr, addr + len) at the task's word: the display's rings and
  // framebuffer, the sound card's samples, the drives' buffers and names.
  // The device does what the task could have done itself, or the task
  // takes the fault it would have taken -- an IOMMU's check, so that no
  // device is a way round the task's page tables (docs/c4ix-mmu.md 11).
  dmaCheck(addr, len, write) {
    const a = this.m.arena;
    const pa = dmaPhys(a.i32, a.size, this.ptbr, addr, len, write);
    if (pa >= 0) return pa;
    this.refuse(dmaBad, dmaKind(write));
    throw FAULT;
  }

  // The bus, for user mode: a device it reaches checks the addresses it is
  // given against this task's space (dmaCheck) while the access lasts
  busRd32(x) {
    const a = this.m.arena;
    a.dmaCtx = this;
    try { return this.lowRd32(x); } finally { a.dmaCtx = null; }
  }
  busWr32(x, v) {
    const a = this.m.arena;
    a.dmaCtx = this;
    try { this.m.bus.write32(x, v | 0); } finally { a.dmaCtx = null; }
  }
  busWr8(x, v) {
    const a = this.m.arena;
    a.dmaCtx = this;
    try { this.m.bus.write8(x, v & 0xff); } finally { a.dmaCtx = null; }
  }

  // ---------------------------------------------------------------
  // Page 0 in user mode: the bus, as the interpreter's slow path has it,
  // where the IOPM allows (section 7). Returns false when it refuses.
  lowOk(x, kind) {
    x = x >>> 0;
    const io = this.iopm;
    if (x < 0x100) return (io & IOPM.ROM) !== 0 && kind === FK.READ;
    if (x < 0x200) return (io & IOPM.DEV) !== 0;
    if (x < 0x400) return (io & IOPM.ROM) !== 0 && kind === FK.READ;
    if (x < 0x480) return (io & IOPM.GUI) !== 0;
    if (x < 0x500) return (io & IOPM.SND) !== 0;
    if (x >= LOCAL_BASE && x < LOCAL_END) return (io & (kind === FK.READ ? IOPM.LOCAL : IOPM.LOCALW)) !== 0;
    return false;
  }
  lowRd32(x) {
    const m = this.m;
    if (m.smp && x >= LOCAL_BASE && x < LOCAL_END && (x & 3) === 0) return m.local[(x - LOCAL_BASE) >> 2] | 0;
    return m.bus.read32(x) | 0;
  }
  lowRd8(x) {
    const m = this.m;
    if (m.smp && x >= LOCAL_BASE && x < LOCAL_END) return (m.localU8[x - LOCAL_BASE] << 24) >> 24;
    return m.bus.read8s(x);
  }

  // ---------------------------------------------------------------
  // Loads and stores for everything but the loop's inline paths: sys(),
  // the FPU, OPCD. In user mode c4m.js's ld32 and friends come here. A
  // fault throws FAULT, which the user loop catches.
  ld32(x) {
    x = x | 0;
    if ((x >>> 0) < 0x1000) {
      if (!this.lowOk(x, FK.READ)) { this.refuse(x, FK.READ | FK.PRESENT); throw FAULT; }
      return this.busRd32(x);
    }
    if (x & 3) return (this.ld8(x) & 255) | ((this.ld8(x + 1) & 255) << 8) | ((this.ld8(x + 2) & 255) << 16) | (this.ld8(x + 3) << 24);
    const p = this.page(x, FK.READ);
    if (p < 0) throw FAULT;
    return this.m.i32[(p | (x & 4095)) >> 2] | 0;
  }
  ld8(x) {
    x = x | 0;
    if ((x >>> 0) < 0x1000) {
      if (!this.lowOk(x, FK.READ)) { this.refuse(x, FK.READ | FK.PRESENT); throw FAULT; }
      return this.lowRd8(x);
    }
    const p = this.page(x, FK.READ);
    if (p < 0) throw FAULT;
    return (this.m.u8[p | (x & 4095)] << 24) >> 24;
  }
  st32(x, v) {
    x = x | 0;
    const m = this.m;
    if ((x >>> 0) < 0x1000) {
      if (!this.lowOk(x, FK.WRITE)) { this.refuse(x, FK.WRITE | FK.PRESENT); throw FAULT; }
      if (m.smp && x >= LOCAL_BASE && x < LOCAL_END && (x & 3) === 0) { m.local[(x - LOCAL_BASE) >> 2] = v; return; }
      if (isRom(x, 4)) { m.romStore(x, v); return; }
      this.busWr32(x, v);
      return;
    }
    if (x & 3) { for (let k = 0; k < 4; k++) this.st8(x + k, v >> (8 * k)); return; }
    const p = this.page(x, FK.WRITE);
    if (p < 0) throw FAULT;
    m.i32[(p | (x & 4095)) >> 2] = v;
  }
  st8(x, v) {
    x = x | 0;
    const m = this.m;
    if ((x >>> 0) < 0x1000) {
      if (!this.lowOk(x, FK.WRITE)) { this.refuse(x, FK.WRITE | FK.PRESENT); throw FAULT; }
      if (m.smp && x >= LOCAL_BASE && x < LOCAL_END) { m.localU8[x - LOCAL_BASE] = v; return; }
      if (isRom(x, 1)) { m.romStore(x, v & 0xff); return; }
      this.busWr8(x, v);
      return;
    }
    const p = this.page(x, FK.WRITE);
    if (p < 0) throw FAULT;
    m.u8[p | (x & 4095)] = v;
  }

  // A string in user memory, as Machine.cstring gives it: a copy
  cstring(addr, max = 1 << 20) {
    const out = [];
    for (let k = 0; k < max; k++) {
      const b = this.ld8(addr + k) & 255;
      if (!b) break;
      out.push(b);
    }
    return Uint8Array.from(out);
  }

  // The block opcodes in user mode, a page at a time: c4m.js sys() hands
  // MSET, MCMP and MCPY here. A page the task may not use faults part way,
  // as a store loop would; what was done before it stays done.
  block(op) {
    const m = this.m, sp = m.sp, u8 = m.u8;
    if (op === OP.MSET) {
      const d = this.ld32(sp + 8), v = this.ld32(sp + 4) & 0xff, n = this.ld32(sp);
      for (let k = 0; k < n;) {
        const x = (d + k) | 0;
        if ((x >>> 0) < 0x1000) { this.st8(x, v); k++; continue; }
        const run = Math.min(n - k, 4096 - (x & 4095));
        const p = this.page(x, FK.WRITE);
        if (p < 0) throw FAULT;
        u8.fill(v, p | (x & 4095), (p | (x & 4095)) + run);
        k += run;
      }
      m.a = d;
      return STOP.BUDGET;
    }
    if (op === OP.MCMP) {
      const pa = this.ld32(sp + 8), qa = this.ld32(sp + 4), n = this.ld32(sp);
      let r = 0;
      for (let k = 0; k < n; k++) {
        const x = (this.ld8(pa + k) & 255) - (this.ld8(qa + k) & 255);
        if (x) { r = x; break; }
      }
      m.a = r;
      return STOP.BUDGET;
    }
    // MCPY: forward, byte by byte when the ranges overlap that way (as
    // c4m.js does), else a run at a time within both pages
    const d = this.ld32(sp + 8), s = this.ld32(sp + 4), n = this.ld32(sp);
    if (n > 0) {
      if (d > s && d < s + n) {
        for (let k = 0; k < n; k++) this.st8(d + k, this.ld8(s + k));
      } else {
        for (let k = 0; k < n;) {
          const x = (d + k) | 0, y = (s + k) | 0;
          if ((x >>> 0) < 0x1000 || (y >>> 0) < 0x1000) { this.st8(x, this.ld8(y)); k++; continue; }
          const run = Math.min(n - k, 4096 - (x & 4095), 4096 - (y & 4095));
          const ps = this.page(y, FK.READ);
          if (ps < 0) throw FAULT;
          const pd = this.page(x, FK.WRITE);
          if (pd < 0) throw FAULT;
          u8.copyWithin(pd | (x & 4095), ps | (y & 4095), (ps | (y & 4095)) + run);
          k += run;
        }
      }
    }
    m.a = d;
    return STOP.BUDGET;
  }

  // OPCD of one of the loop's own opcodes without an operand, in user
  // mode: c4m.js opcd() over the translated loads and stores
  opcd(op) {
    const m = this;
    const mm = this.m;
    let sp = mm.sp, a = mm.a, x = 0;
    switch (op) {
      case OP.LEV: {
        const bp = mm.bp;
        const nbp = m.ld32(bp), npc = m.ld32(bp + 4);
        mm.bp = nbp; mm.pc = npc; sp = bp + 8;
        break;
      }
      case OP.LI: a = m.ld32(a); break;
      case OP.LC: a = m.ld8(a); break;
      case OP.SI: x = m.ld32(sp); m.st32(x, a); sp += 4; break;
      case OP.SC: x = m.ld32(sp); m.st8(x, a); sp += 4; a = (a << 24) >> 24; break;
      case OP.PSH: m.st32(sp - 4, a); sp -= 4; break;
      case OP.LIP: a = m.ld32(a); m.st32(sp - 4, a); sp -= 4; break;
      case OP.ADDL: x = (m.ld32(sp) + a) | 0; a = m.ld32(x); sp += 4; break;
      case OP.POPA: a = m.ld32(sp); sp += 4; break;
      case OP.JMPA: mm.pc = a; break;
      case OP._JMP: mm.pc = m.ld32(sp); sp += 4; break;
      case OP._ADJ: sp = (sp + (m.ld32(sp) << 2)) | 0; break;
      case OP.C4CY: a = mm.cycle | 0; break;
      default: {
        const t = m.ld32(sp); sp += 4;
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
    mm.sp = sp; mm.a = a;
  }

  // The opcode at pc in user mode, for boundary()'s "TLEV is never
  // interrupted": -1 if it cannot be read
  peek(pc) {
    if ((pc >>> 0) < 0x1000) return (pc >>> 0) === TLEV_ADDR ? OP.TLEV : -1;
    const v = pc >>> 12;
    const p = v < LIMIT ? this.ex[v] : -1;
    return p >= 0 ? this.m.i32[(p | (pc & 4095)) >> 2] : -1;
  }

  // ---------------------------------------------------------------
  // The loop's slow paths. Each is called with the machine's registers
  // saved as the instruction found them (pc its first word), returns
  // what the access gives, and on a fault raises it: the caller looks at
  // m.mode, which the trap has made 0.

  // The stack window: [sLo, sLo + sLen) virtual, at sOff from physical,
  // pages user mode may read and write, around sp. Pushes and pops do not
  // look at it: each moves sp a word at most, so the top of the loop
  // counts how many instructions cannot take sp out of it, and comes
  // back when they have run, or when an instruction moved sp further
  // (ENT, ADJ, LEV, _ADJ) or did anything else that ends the count. Then
  // this keeps the window if it still has a word either side of sp; else
  // makes one there, joined to the old one when they are one run, and
  // grown by up to STACK_GROW pages each way while the next page follows
  // on in physical memory. False if a word either side of sp is not the
  // task's to write: its stack is not where sp says.
  sFix(sp) {
    const lo = (sp - 4) & -4096, hi = sp & -4096;
    const p = this.rwPage(lo);
    if (p < 0) return false;
    const off = p - lo;
    let wlo = lo, whi = lo + 4096;
    if (hi !== lo) {
      const q = this.rwPage(hi);
      if (q < 0 || q - hi !== off) return false;
      whi = hi + 4096;
    }
    if (this.sLen && off === this.sOff && wlo <= this.sLo + this.sLen && whi >= this.sLo) {
      if (this.sLo < wlo) wlo = this.sLo;
      if (this.sLo + this.sLen > whi) whi = this.sLo + this.sLen;
    }
    for (let k = 0; k < STACK_GROW; k++) {
      const q = this.rwPage(wlo - 4096);
      if (q < 0 || q - (wlo - 4096) !== off) break;
      wlo -= 4096;
    }
    for (let k = 0; k < STACK_GROW; k++) {
      const q = this.rwPage(whi);
      if (q < 0 || q - whi !== off) break;
      whi += 4096;
    }
    this.sLo = wlo; this.sLen = whi - wlo; this.sOff = off;
    return true;
  }

  // The physical page at va if user mode may read and write it, filled
  // (and made dirty); -1, raising nothing, if not
  rwPage(va) {
    if ((va >>> 0) < 0x1000) return -1;
    const v = va >>> 12;
    if (v < LIMIT && this.wr[v] >= 0) return this.wr[v];
    const pte = this.probe(va);
    if ((pte & (PTE.P | PTE.U | PTE.W)) !== (PTE.P | PTE.U | PTE.W)) return -1;
    return this.xlate(va, FK.WRITE);
  }

  // sp is outside the task's writable memory (sFix said no): the fault
  // the next push or pop would take, raised before the instruction runs
  stackFault(sp) {
    if (this.rwPage(sp - 4) < 0) this.refuse(sp - 4, FK.WRITE);
    else this.refuse(sp, FK.READ);
    this.raise();
  }

  // A frame word (bp-relative) or a stack word outside the window: the
  // page, without moving the window, or a fault
  sRd(y) {
    try { return this.ld32(y); } catch (e) { if (e !== FAULT) throw e; this.raise(); return 0; }
  }
  sWr(y, v) {
    try { this.st32(y, v); } catch (e) { if (e !== FAULT) throw e; this.raise(); }
  }

  // The code window: the run of pages around pc that may be run, mapped
  // next to each other in physical memory too -- a program's text, up to
  // CODE_GROW pages each way. cLen leaves out its last word, so that an
  // instruction that starts inside it has its operand inside too; the
  // loop refills the window when pc is past that, and an instruction in
  // the last word is looked at here: its operand, if it has one, must be
  // on the next page, mapped next to this one. The last CODE_WINS windows
  // are kept, so that going back and forth between a program and its
  // libraries finds them made. Raises a fault (the caller looks at
  // m.mode) if pc may not be run.
  cFill(pc) {
    const m = this.m, w = this.cwins;
    if ((pc >>> 0) < 0x1000) {
      // the ROM's TLEV word, where trap frames return; nothing else runs,
      // whether or not the IOPM lets the task read the ROM (a null
      // pointer's reads: section 7). The window is that word alone: cLen
      // 0, so the next fetch looks.
      if ((pc >>> 0) === TLEV_ADDR || (pc >>> 0) === CPU_HALT_ADDR) {
        this.cLo = pc; this.cOff = 0; this.cLen = 0;
        return;
      }
      this.refuse(pc, FK.FETCH); this.raise(); return;
    }
    for (let k = 0; k < this.ncwins; k++) {
      if ((pc - w[3 * k]) >>> 0 < w[3 * k + 1]) {
        this.cLo = w[3 * k]; this.cLen = w[3 * k + 1]; this.cOff = w[3 * k + 2];
        return;
      }
    }
    const p = this.page(pc, FK.FETCH);
    if (p < 0) { this.raise(); return; }
    const vp = pc & -4096, off = p - vp;
    let lo = vp, hi = vp + 4096;
    for (let k = 0; k < CODE_GROW; k++) {
      const q = this.xPage(lo - 4096);
      if (q < 0 || q - (lo - 4096) !== off) break;
      lo -= 4096;
    }
    for (let k = 0; k < CODE_GROW; k++) {
      const q = this.xPage(hi);
      if (q < 0 || q - hi !== off) break;
      hi += 4096;
    }
    this.cLo = lo; this.cOff = off; this.cLen = hi - lo - 4;
    // kept, the newest first
    const n = this.ncwins < CODE_WINS ? this.ncwins++ : CODE_WINS - 1;
    w.copyWithin(3, 0, 3 * n);
    w[0] = lo; w[1] = hi - lo - 4; w[2] = off;
    // the last word of the window, whose operand would be past its end
    if ((pc - this.cLo) >>> 0 >= this.cLen && hasOperand(m.i32[(p | (pc & 4095)) >> 2])) {
      const q = this.page(pc + 4, FK.FETCH);
      if (q < 0) { this.raise(); return; }
      // an operand across a page boundary into memory that is not next
      // to the opcode's (docs/c4ix-mmu.md section 5): a fault
      this.refuse(pc + 4, FK.FETCH | 16); this.raise();
    }
  }

  // A load or store through a pointer that missed the arrays (or was
  // unaligned): the page, a fill, page 0's bus, or a fault
  pRd32(x) {
    try { return this.ld32(x); } catch (e) { if (e !== FAULT) throw e; this.raise(); return 0; }
  }
  pRd8(x) {
    try { return this.ld8(x); } catch (e) { if (e !== FAULT) throw e; this.raise(); return 0; }
  }
  pWr32(x, v) {
    try { this.st32(x, v); } catch (e) { if (e !== FAULT) throw e; this.raise(); }
  }
  pWr8(x, v) {
    try { this.st8(x, v); } catch (e) { if (e !== FAULT) throw e; this.raise(); }
  }

  // ---------------------------------------------------------------
  // Up to maxCycles, in whichever loop the mode calls for. Kernel mode is
  // the interpreter's own loop; a TLEV into user mode leaves it with
  // STOP.SWITCH, and a trap leaves this one the same way.
  run(maxCycles) {
    const m = this.m;
    const end = m.cycle + maxCycles;
    for (;;) {
      const left = end - m.cycle;
      if (left <= 0) return STOP.BUDGET;
      const s = !(m.mode && this.on) ? m.runPlain(left)
        : this.general || !runUserId ? this.runUser(left) : runUserId.call(this, left);
      if (s !== STOP.SWITCH) return s;
    }
  }

  // Calls into sys() from user mode: a fault anywhere in it comes back as
  // FAULT, raised here from the instruction's own registers
  sysUser(op, ip, sp, bp, a) {
    const m = this.m;
    try {
      return m.sys(op);
    } catch (e) {
      if (e !== FAULT) throw e;
      m.pc = ip; m.sp = sp; m.bp = bp; m.a = a;
      this.fpc = ip;
      this.raise();
      return STOP.BUDGET;
    }
  }
  opcdUser(op, ip, sp, bp, a) {
    const m = this.m;
    try {
      this.opcd(op);
    } catch (e) {
      if (e !== FAULT) throw e;
      m.pc = ip; m.sp = sp; m.bp = bp; m.a = a;
      this.fpc = ip;
      this.raise();
    }
  }

  // The user-mode loop: c4m.js run()'s, with every access translated. Its
  // cases follow run()'s one for one. What differs:
  //  - the fetch reads through the code window, and pushes and pops
  //    through the stack window, without looking: the top of the loop
  //    counts how far both are good for (cFill, sFix), and a jump, or an
  //    instruction that moves sp more than a word, ends the count (lim);
  //  - frame words (bp-relative) are looked up in the stack window, and
  //    pointers in the arrays (rd, wr), with a slow path past either;
  //  - a slow path saves the registers as the instruction found them (pc
  //    its first word, sp before any pop) and leaves the loop when a
  //    fault has taken the machine into kernel mode.
  runUser(maxCycles) {
    const m = this.m, dev = m.dev, i32 = m.i32, u8 = m.u8;
    const rd = this.rd, wr = this.wr;
    if (dev.halted) return STOP.HALT;
    let pc = m.pc | 0, sp = m.sp | 0, bp = m.bp | 0, a = m.a | 0;
    let cycle = m.cycle;
    const end = cycle + maxCycles;
    let interval = m.cycleInterval | 0;
    let nextIrq = interval > 0 ? (Math.floor(cycle / interval) + 1) * interval : 0;
    let chk = m.needChecks();
    let nextChk = 0;
    let lim = -1;
    let stop = STOP.BUDGET;
    let op = 0, x = 0, y = 0, p = 0, v = 0;
    let sLo = this.sLo, sLen = this.sLen, sOff = this.sOff;
    let cLo = this.cLo, cLen = this.cLen, cOff = this.cOff;
    m.vm = true;

    loop: for (;;) {
      if (++cycle > lim) {
        if (cycle > end) { --cycle; break; }
        if (chk && cycle >= nextChk) {
          m.pc = pc; m.sp = sp; m.bp = bp; m.a = a; m.cycle = cycle;
          nextChk = m.boundary(cycle, nextIrq);
          nextIrq = m.bNextIrq;
          if (m.bFired) {
            pc = m.pc; sp = m.sp; bp = m.bp; a = m.a;
            interval = m.cycleInterval | 0;
            chk = m.needChecks();
            if (!m.mode || dev.halted) { stop = dev.halted ? STOP.HALT : STOP.SWITCH; break loop; }
          }
        }
        // The code window: pc has left it, or may have
        if ((pc - cLo) >>> 0 >= cLen) {
          m.pc = pc; m.sp = sp; m.bp = bp; m.a = a; m.cycle = cycle;
          this.cFill(pc);
          if (!m.mode || dev.halted) { stop = dev.halted ? STOP.HALT : STOP.SWITCH; break loop; }
          cLo = this.cLo; cLen = this.cLen; cOff = this.cOff;
        }
        // The stack window: a word either side of sp
        x = sp - sLo;
        if (x < 4 || x > sLen - 4) {
          m.pc = pc; m.sp = sp; m.bp = bp; m.a = a; m.cycle = cycle;
          if (!this.sFix(sp)) {
            this.stackFault(sp);
            stop = dev.halted ? STOP.HALT : STOP.SWITCH;
            break loop;
          }
          sLo = this.sLo; sLen = this.sLen; sOff = this.sOff;
          x = sp - sLo;
        }
        // How many instructions run before the next look: up to the
        // checks and the budget; while no instruction, 8 bytes at most,
        // can run off the code window; and while no push or pop, a word
        // each, can run off the stack window
        lim = chk && nextChk - 1 < end ? nextChk - 1 : end;
        y = cycle + ((cLo + cLen + 4 - pc) >> 3) - 1;
        if (y < lim) lim = y;
        y = cycle + ((x < sLen - x ? x : sLen - x) >> 2) - 1;
        if (y < lim) lim = y;
        if (lim < cycle) lim = cycle;
      }

      op = i32[(pc + cOff) >> 2];
      pc += 4;

      switch (op) {
        case 0:  /* LEA */ a = (bp + (i32[(pc + cOff) >> 2] << 2)) | 0; pc += 4; break;
        case 1:  /* IMM */ a = i32[(pc + cOff) >> 2]; pc += 4; break;
        case 2:  /* JMP */ pc = i32[(pc + cOff) >> 2]; if ((pc - cLo) >>> 0 < cLen) { y = cycle + ((cLo + cLen + 4 - pc) >> 3); if (y < lim) lim = y; } else lim = -1; break;
        case 3:  /* JSR */ sp -= 4; i32[(sp + sOff) >> 2] = pc + 4; pc = i32[(pc + cOff) >> 2]; if ((pc - cLo) >>> 0 < cLen) { y = cycle + ((cLo + cLen + 4 - pc) >> 3); if (y < lim) lim = y; } else lim = -1; break;
        case 4:  /* BZ  */ if (a) pc += 4; else { pc = i32[(pc + cOff) >> 2]; if ((pc - cLo) >>> 0 < cLen) { y = cycle + ((cLo + cLen + 4 - pc) >> 3); if (y < lim) lim = y; } else lim = -1; } break;
        case 5:  /* BNZ */ if (a) { pc = i32[(pc + cOff) >> 2]; if ((pc - cLo) >>> 0 < cLen) { y = cycle + ((cLo + cLen + 4 - pc) >> 3); if (y < lim) lim = y; } else lim = -1; } else pc += 4; break;
        case 6:  /* ENT */
          sp -= 4; i32[(sp + sOff) >> 2] = bp; bp = sp;
          sp = (sp - (i32[(pc + cOff) >> 2] << 2)) | 0; pc += 4;
          x = sp - sLo; if (x < 4 || x > sLen - 4) lim = -1; else { y = cycle + ((x < sLen - x ? x : sLen - x) >> 2); if (y < lim) lim = y; }
          break;
        case 7:  /* ADJ */ sp = (sp + (i32[(pc + cOff) >> 2] << 2)) | 0; pc += 4; x = sp - sLo; if (x < 4 || x > sLen - 4) lim = -1; else { y = cycle + ((x < sLen - x ? x : sLen - x) >> 2); if (y < lim) lim = y; } break;
        case 8:  /* LEV */
          if ((bp - sLo) >>> 0 < sLen - 4) { x = i32[(bp + sOff) >> 2]; y = i32[(bp + 4 + sOff) >> 2]; }
          else {
            m.pc = pc - 4; m.sp = sp; m.bp = bp; m.a = a; m.cycle = cycle;
            x = this.sRd(bp);
            if (!m.mode) { stop = STOP.SWITCH; break loop; }
            y = this.sRd(bp + 4);
            if (!m.mode) { stop = STOP.SWITCH; break loop; }
          }
          sp = bp + 8; bp = x; pc = y;
          x = sp - sLo; if (x < 4 || x > sLen - 4) lim = -1; else { y = cycle + ((x < sLen - x ? x : sLen - x) >> 2); if (y < lim) lim = y; }
          if ((pc - cLo) >>> 0 < cLen) { y = cycle + ((cLo + cLen + 4 - pc) >> 3); if (y < lim) lim = y; } else lim = -1;
          break;
        case 9:  /* LI  */
          v = a >>> 12; p = v < LIMIT ? rd[v] : -1;
          if (p >= 0 && (a & 3) === 0) a = i32[(p | (a & 4095)) >> 2];
          else {
            m.pc = pc - 4; m.sp = sp; m.bp = bp; m.a = a; m.cycle = cycle;
            a = this.pRd32(a);
            if (!m.mode) { stop = STOP.SWITCH; break loop; }
          }
          break;
        case 10: /* LC  */
          v = a >>> 12; p = v < LIMIT ? rd[v] : -1;
          if (p >= 0) a = (u8[p | (a & 4095)] << 24) >> 24;
          else {
            m.pc = pc - 4; m.sp = sp; m.bp = bp; m.a = a; m.cycle = cycle;
            a = this.pRd8(a);
            if (!m.mode) { stop = STOP.SWITCH; break loop; }
          }
          break;
        case 11: /* SI  */
          x = i32[(sp + sOff) >> 2];
          v = x >>> 12; p = v < LIMIT ? wr[v] : -1;
          if (p >= 0 && (x & 3) === 0) { i32[(p | (x & 4095)) >> 2] = a; sp += 4; }
          else {
            m.pc = pc - 4; m.sp = sp; m.bp = bp; m.a = a; m.cycle = cycle;
            this.pWr32(x, a);
            if (!m.mode || dev.halted) { stop = dev.halted ? STOP.HALT : STOP.SWITCH; break loop; }
            sp += 4;
            // a store into the device window can change what is pending
            interval = m.cycleInterval | 0;
            nextIrq = interval > 0 ? (Math.floor(cycle / interval) + 1) * interval : 0;
            chk = m.needChecks(); nextChk = 0; lim = -1;
          }
          break;
        case 12: /* SC  */
          x = i32[(sp + sOff) >> 2];
          v = x >>> 12; p = v < LIMIT ? wr[v] : -1;
          if (p >= 0) { u8[p | (x & 4095)] = a; sp += 4; }
          else {
            m.pc = pc - 4; m.sp = sp; m.bp = bp; m.a = a; m.cycle = cycle;
            this.pWr8(x, a);
            if (!m.mode || dev.halted) { stop = dev.halted ? STOP.HALT : STOP.SWITCH; break loop; }
            sp += 4;
            interval = m.cycleInterval | 0;
            nextIrq = interval > 0 ? (Math.floor(cycle / interval) + 1) * interval : 0;
            chk = m.needChecks(); nextChk = 0; lim = -1;
          }
          a = (a << 24) >> 24;
          break;
        case 13: /* PSH */ sp -= 4; i32[(sp + sOff) >> 2] = a; break;
        case 14: /* OR  */ a = i32[(sp + sOff) >> 2] | a; sp += 4; break;
        case 15: /* XOR */ a = i32[(sp + sOff) >> 2] ^ a; sp += 4; break;
        case 16: /* AND */ a = i32[(sp + sOff) >> 2] & a; sp += 4; break;
        case 17: /* EQ  */ a = i32[(sp + sOff) >> 2] === a ? 1 : 0; sp += 4; break;
        case 18: /* NE  */ a = i32[(sp + sOff) >> 2] !== a ? 1 : 0; sp += 4; break;
        case 19: /* LT  */ a = i32[(sp + sOff) >> 2] < a ? 1 : 0; sp += 4; break;
        case 20: /* GT  */ a = i32[(sp + sOff) >> 2] > a ? 1 : 0; sp += 4; break;
        case 21: /* LE  */ a = i32[(sp + sOff) >> 2] <= a ? 1 : 0; sp += 4; break;
        case 22: /* GE  */ a = i32[(sp + sOff) >> 2] >= a ? 1 : 0; sp += 4; break;
        case 23: /* SHL */ a = i32[(sp + sOff) >> 2] << a; sp += 4; break;
        case 24: /* SHR */ a = i32[(sp + sOff) >> 2] >> a; sp += 4; break;
        case 25: /* ADD */ a = (i32[(sp + sOff) >> 2] + a) | 0; sp += 4; break;
        case 26: /* SUB */ a = (i32[(sp + sOff) >> 2] - a) | 0; sp += 4; break;
        case 27: /* MUL */ a = Math.imul(i32[(sp + sOff) >> 2], a); sp += 4; break;
        case 28: /* DIV */ a = a === 0 ? 0 : (i32[(sp + sOff) >> 2] / a) | 0; sp += 4; break;
        case 29: /* MOD */ a = a === 0 ? 0 : (i32[(sp + sOff) >> 2] % a) | 0; sp += 4; break;

        case 79: /* LDL  */
          y = (bp + (i32[(pc + cOff) >> 2] << 2)) | 0;
          if ((y - sLo) >>> 0 < sLen) a = i32[(y + sOff) >> 2];
          else {
            m.pc = pc - 4; m.sp = sp; m.bp = bp; m.a = a; m.cycle = cycle;
            a = this.sRd(y);
            if (!m.mode) { stop = STOP.SWITCH; break loop; }
          }
          pc += 4;
          break;
        case 80: /* LDG  */
          x = i32[(pc + cOff) >> 2];
          v = x >>> 12; p = v < LIMIT ? rd[v] : -1;
          if (p >= 0 && (x & 3) === 0) a = i32[(p | (x & 4095)) >> 2];
          else {
            m.pc = pc - 4; m.sp = sp; m.bp = bp; m.a = a; m.cycle = cycle;
            a = this.pRd32(x);
            if (!m.mode) { stop = STOP.SWITCH; break loop; }
          }
          pc += 4;
          break;
        case 81: /* PSHL */
          y = (bp + (i32[(pc + cOff) >> 2] << 2)) | 0;
          if ((y - sLo) >>> 0 < sLen) a = i32[(y + sOff) >> 2];
          else {
            m.pc = pc - 4; m.sp = sp; m.bp = bp; m.a = a; m.cycle = cycle;
            a = this.sRd(y);
            if (!m.mode) { stop = STOP.SWITCH; break loop; }
          }
          sp -= 4; i32[(sp + sOff) >> 2] = a; pc += 4;
          break;
        case 82: /* PSHG */
          x = i32[(pc + cOff) >> 2];
          v = x >>> 12; p = v < LIMIT ? rd[v] : -1;
          if (p >= 0 && (x & 3) === 0) a = i32[(p | (x & 4095)) >> 2];
          else {
            m.pc = pc - 4; m.sp = sp; m.bp = bp; m.a = a; m.cycle = cycle;
            a = this.pRd32(x);
            if (!m.mode) { stop = STOP.SWITCH; break loop; }
          }
          sp -= 4; i32[(sp + sOff) >> 2] = a; pc += 4;
          break;
        case 83: /* LEAP */ a = (bp + (i32[(pc + cOff) >> 2] << 2)) | 0; pc += 4; sp -= 4; i32[(sp + sOff) >> 2] = a; break;
        case 84: /* IMMP */ a = i32[(pc + cOff) >> 2]; pc += 4; sp -= 4; i32[(sp + sOff) >> 2] = a; break;
        case 85: /* LIP  */
          v = a >>> 12; p = v < LIMIT ? rd[v] : -1;
          if (p >= 0 && (a & 3) === 0) a = i32[(p | (a & 4095)) >> 2];
          else {
            m.pc = pc - 4; m.sp = sp; m.bp = bp; m.a = a; m.cycle = cycle;
            a = this.pRd32(a);
            if (!m.mode) { stop = STOP.SWITCH; break loop; }
          }
          sp -= 4; i32[(sp + sOff) >> 2] = a;
          break;
        case 86: /* ADDL */
          x = (i32[(sp + sOff) >> 2] + a) | 0;
          v = x >>> 12; p = v < LIMIT ? rd[v] : -1;
          if (p >= 0 && (x & 3) === 0) a = i32[(p | (x & 4095)) >> 2];
          else {
            m.pc = pc - 4; m.sp = sp; m.bp = bp; m.a = a; m.cycle = cycle;
            a = this.pRd32(x);
            if (!m.mode) { stop = STOP.SWITCH; break loop; }
          }
          sp += 4;
          break;
        case 87: /* STL  */
          y = (bp + (i32[(pc + cOff) >> 2] << 2)) | 0;
          if ((y - sLo) >>> 0 < sLen) i32[(y + sOff) >> 2] = a;
          else {
            m.pc = pc - 4; m.sp = sp; m.bp = bp; m.a = a; m.cycle = cycle;
            this.sWr(y, a);
            if (!m.mode) { stop = STOP.SWITCH; break loop; }
          }
          pc += 4;
          break;
        case 88: /* POPA */ a = i32[(sp + sOff) >> 2]; sp += 4; break;

        case 61: /* JSRI */
          x = i32[(pc + cOff) >> 2];
          v = x >>> 12; p = v < LIMIT ? rd[v] : -1;
          if (p >= 0 && (x & 3) === 0) x = i32[(p | (x & 4095)) >> 2];
          else {
            m.pc = pc - 4; m.sp = sp; m.bp = bp; m.a = a; m.cycle = cycle;
            x = this.pRd32(x);
            if (!m.mode) { stop = STOP.SWITCH; break loop; }
          }
          sp -= 4; i32[(sp + sOff) >> 2] = pc + 4; pc = x; if ((pc - cLo) >>> 0 < cLen) { y = cycle + ((cLo + cLen + 4 - pc) >> 3); if (y < lim) lim = y; } else lim = -1;
          break;
        case 62: /* JSRS */
          y = (bp + (i32[(pc + cOff) >> 2] << 2)) | 0;
          if ((y - sLo) >>> 0 < sLen) x = i32[(y + sOff) >> 2];
          else {
            m.pc = pc - 4; m.sp = sp; m.bp = bp; m.a = a; m.cycle = cycle;
            x = this.sRd(y);
            if (!m.mode) { stop = STOP.SWITCH; break loop; }
          }
          sp -= 4; i32[(sp + sOff) >> 2] = pc + 4; pc = x; if ((pc - cLo) >>> 0 < cLen) { y = cycle + ((cLo + cLen + 4 - pc) >> 3); if (y < lim) lim = y; } else lim = -1;
          break;
        case 63: /* JMPA */ pc = a; if ((pc - cLo) >>> 0 < cLen) { y = cycle + ((cLo + cLen + 4 - pc) >> 3); if (y < lim) lim = y; } else lim = -1; break;
        case 49: /* _JMP */ pc = i32[(sp + sOff) >> 2]; sp += 4; if ((pc - cLo) >>> 0 < cLen) { y = cycle + ((cLo + cLen + 4 - pc) >> 3); if (y < lim) lim = y; } else lim = -1; break;
        case 50: /* _ADJ */ sp = (sp + (i32[(sp + sOff) >> 2] << 2)) | 0; x = sp - sLo; if (x < 4 || x > sLen - 4) lim = -1; else { y = cycle + ((x < sLen - x ? x : sLen - x) >> 2); if (y < lim) lim = y; } break;
        case 52: /* C4CY */ a = cycle | 0; break;

        case 48: /* OPCD */
          op = i32[(sp + sOff) >> 2];
          if (hasOperand(op)) {
            // as run() does: a (usually missed) OPV, then C4CY
            m.pc = pc; m.sp = sp; m.bp = bp; m.a = a; m.cycle = cycle;
            m.opvTrap(op);
            pc = m.pc; sp = m.sp; bp = m.bp; a = m.a;
            if (!m.mode) { stop = STOP.SWITCH; break loop; }
            a = cycle | 0;
            lim = -1;
            break;
          }
          if (isPlainOp(op)) {
            m.pc = pc; m.sp = sp; m.bp = bp; m.a = a; m.cycle = cycle;
            this.opcdUser(op, pc - 4, sp, bp, a);
            if (!m.mode || dev.halted) { stop = dev.halted ? STOP.HALT : STOP.SWITCH; break loop; }
            pc = m.pc; sp = m.sp; bp = m.bp; a = m.a;
            interval = m.cycleInterval | 0;
            nextIrq = interval > 0 ? (Math.floor(cycle / interval) + 1) * interval : 0;
            chk = m.needChecks(); nextChk = 0; lim = -1;
            break;
          }
          // falls through

        default: {
          m.pc = pc; m.sp = sp; m.bp = bp; m.a = a; m.cycle = cycle;
          const s = this.sysUser(op, pc - 4, sp, bp, a);
          pc = m.pc; sp = m.sp; bp = m.bp; a = m.a;
          cycle = m.cycle;
          interval = m.cycleInterval | 0;
          nextIrq = interval > 0 ? (Math.floor(cycle / interval) + 1) * interval : 0;
          chk = m.needChecks(); nextChk = 0; lim = -1;
          if (s !== STOP.BUDGET) { stop = s; break loop; }
          if (dev.halted) { stop = STOP.HALT; break loop; }
          if (!m.mode) { stop = STOP.SWITCH; break loop; }
        }
      }
    }

    // A fault or a trap left the registers in the machine already
    if (m.mode && m.vm) { m.pc = pc; m.sp = sp; m.bp = bp; m.a = a; }
    m.cycle = cycle;
    this.sLo = sLo; this.sLen = sLen; this.sOff = sOff;
    this.cLo = cLo; this.cLen = cLen; this.cOff = cOff;
    m.vm = false;
    if (dev.halted) stop = STOP.HALT;
    return stop;
  }
}

// The identity loop. An identity map -- C4IX's, every page at its own
// address -- makes every window's offset 0, and the adds that apply them
// are 5% of a user-heavy run on helios (docs/c4ix-mmu.md 12). So the
// dispatcher runs a copy of runUser without them: made here from
// runUser's own text, so the two cannot drift apart, which hands over to
// runUser for good (general) the first time a window is not identity. A
// page whose policy forbids making code this way runs runUser throughout.
function identityLoop() {
  let src = Mmu.prototype.runUser.toString();
  if (!src.startsWith('runUser(')) return null;
  src = 'function runUserId(' + src.slice('runUser('.length);
  src = src.replace(/\(pc \+ cOff\) >> 2/g, 'pc >> 2')
           .replace(/\((\w+(?: \+ 4)?) \+ sOff\) >> 2/g, '($1) >> 2')
           .replace(/(cLo = this\.cLo; cLen = this\.cLen; cOff = this\.cOff;)/g,
                    '$1 if (cOff !== 0) { this.general = true; stop = STOP.SWITCH; break loop; }')
           .replace(/(sLo = this\.sLo; sLen = this\.sLen; sOff = this\.sOff;)/g,
                    '$1 if (sOff !== 0) { this.general = true; stop = STOP.SWITCH; break loop; }');
  if (/[cs]Off\) >> 2/.test(src)) return null;
  try {
    return new Function('STOP', 'LIMIT', 'hasOperand', 'isPlainOp', `return ${src}`)(STOP, LIMIT, hasOperand, isPlainOp);
  } catch {
    return null;
  }
}
const runUserId = identityLoop();

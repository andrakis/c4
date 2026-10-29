// arena.js - the flat byte-addressed memory of the c4bb machine.
//
// One Int32Array-backed arena replaces c4m's four malloc pools and raw
// host pointers, following the conversion src/oisc4 established: every
// address a program sees is a byte offset into this arena.
//
// Low memory is special:
//   0x0000-0x000F  null guard (reads 0; writes ignored until traps exist)
//   0x0010         TLEV ROM word
//   0x0014         constructor-return sentinel (never executed as code)
//   0x0020-0x003F  firmware vector latches (plain RAM, by convention)
//   0x0100-0x01FF  memory-mapped device registers (see devices.js)
//   0x0200-0x03BC  opcode-name ROM (89 entries x 5 bytes, c4m layout)
//   0x1000         MEM_BASE - loaded images, then heap
//   top            stack, growing down (top 4KB reserved for argv)

export const TLEV_ADDR = 0x10;
export const CONS_RET = 0x14;
export const VEC_MALC = 0x20, VEC_FREE = 0x24, VEC_RALC = 0x28,
             VEC_PRTF = 0x2c, VEC_STRC = 0x30;
export const DEV_BASE = 0x100, DEV_END = 0x200;
export const OPNAMES_ROM = 0x200;
export const MEM_BASE = 0x1000;
export const ARGS_RESERVE = 4096;

// The word of the page table entry for va in the space whose directory
// is at dir (two levels of 1,024 words, libjs/mmu.js), or -1. PIN: the
// bit the host sets there on a page the kernel or a device has used,
// which the kernel then never puts out to swap (C4IX's PTE_PIN).
export const PTE_PIN = 0x200;
export function pteWord(i32, size, dir, va) {
  if (!dir) return -1;
  const pde = i32[(dir >>> 2) + (va >>> 22)] | 0;
  if (!(pde & 1)) return -1;
  const pt = pde & -4096;
  if ((pt >>> 0) + 4096 > size) return -1;
  return (pt >>> 2) + ((va >>> 12) & 1023);
}
// The memory at user address x by the kernel's map (UDIR), the page
// pinned; -1 if it maps nothing there
export function kernelPhys(i32, size, udir, x) {
  const w = pteWord(i32, size, udir, x);
  if (w < 0) return -1;
  const pte = i32[w] | 0;
  if (!(pte & 1) || ((pte & -4096) >>> 0) + 4096 > size) return -1;
  if (!(pte & PTE_PIN)) i32[w] = pte | PTE_PIN;
  return (pte & -4096) | (x & 4095);
}

export class Arena {
  // A size, or a buffer to wrap: a SharedArrayBuffer lets a host on another
  // thread read and write the mailbox rings while the machine runs.
  constructor(sizeOrBuffer = 32 * 1024 * 1024) {
    const buf = typeof sizeOrBuffer === 'number' ? new ArrayBuffer(sizeOrBuffer) : sizeOrBuffer;
    const sizeBytes = buf.byteLength;
    this.size = sizeBytes;
    this.buf = buf;
    this.i32 = new Int32Array(this.buf);
    this.u8 = new Uint8Array(this.buf);
    this.dv = new DataView(this.buf);
    this.io = null;          // devices.js Devices instance
    this.stackTop = sizeBytes - ARGS_RESERVE;
    // Whose word a device is acting on: null for the machine's own (no
    // MMU, or kernel mode), else the user task's MMU while its load or
    // store is at the device (libjs/mmu.js), which checks (dmaCheck)
    this.dmaCtx = null;
    // The user window (libjs/c4m.js kpa): kernel mode hands a device a
    // user task's address as readily as its own. 0: none. udir: the
    // kernel's map of user addresses, 0 for none (an offset, then).
    this.uwin = 0;
    this.udir = 0;
  }

  // ---- what devices do at an address the guest gave them ----
  // Every device that reads or writes guest memory at an address it was
  // handed -- a buffer, a name, a ring, a framebuffer -- comes through
  // these, so that a user task under an MMU can only hand it its own
  // memory: the range is checked against the task's space, and a refusal
  // is the task's page fault (docs/c4ix-mmu.md 11).
  // dma: where the device finds [addr, addr + len), one run of memory --
  // the address itself, or what the task's page tables or the window
  // make of it -- or -1 when there is no such memory. A user task's
  // refusal throws (its page fault).
  dma(addr, len, write) {
    addr = addr >>> 0;
    if (this.dmaCtx) return this.dmaCtx.dmaCheck(addr, len, write);
    if (this.uwin && addr >= this.uwin) {
      if (this.udir) return kernelPhys(this.i32, this.size, this.udir, addr);
      const off = addr - this.uwin;
      return off < this.size ? off : -1;
    }
    return addr;
  }
  // A copy of LEN bytes at addr, a page at a time: past the end, short
  dmaRead(addr, len) {
    addr = addr >>> 0;
    len = Math.max(0, len | 0);
    if (!this.dmaCtx && !(this.uwin && addr >= this.uwin)) {
      return this.u8.slice(addr, Math.min(addr + len, this.size));
    }
    const out = new Uint8Array(len);
    for (let k = 0; k < len;) {
      const va = (addr + k) >>> 0, r = Math.min(len - k, 4096 - (va & 4095));
      const p = this.dma(va, r, false);
      if (p < 0 || p + r > this.size) return out.subarray(0, k);
      out.set(this.u8.subarray(p, p + r), k);
      k += r;
    }
    return out;
  }
  dmaWrite(addr, bytes) {
    addr = addr >>> 0;
    if (!(bytes instanceof Uint8Array)) bytes = Uint8Array.from(bytes);
    const len = bytes.length;
    if (!this.dmaCtx && !(this.uwin && addr >= this.uwin)) {
      if (addr < this.size) this.u8.set(len > this.size - addr ? bytes.subarray(0, this.size - addr) : bytes, addr);
      return;
    }
    for (let k = 0; k < len;) {
      const va = (addr + k) >>> 0, r = Math.min(len - k, 4096 - (va & 4095));
      const p = this.dma(va, r, true);
      if (p < 0 || p + r > this.size) return;
      this.u8.set(bytes.subarray(k, k + r), p);
      k += r;
    }
  }
  dmaCstring(addr, max = 4096) {
    addr = addr >>> 0;
    const out = [];
    let p = -1;
    for (let k = 0; k < max; k++) {
      const va = (addr + k) >>> 0;
      if (p < 0 || (va & 4095) === 0) { p = this.dma(va, 1, false); if (p < 0 || p >= this.size) break; }
      else p++;
      const b = this.u8[p];
      if (!b) break;
      out.push(b);
    }
    return new TextDecoder('latin1').decode(Uint8Array.from(out));
  }

  // Out-of-range accesses read 0 and drop writes, like the aligned
  // typed-array paths already do - a wild program misbehaves the same
  // way everywhere instead of throwing out of the simulator.
  read32(addr) {
    addr = addr >>> 0;
    if (addr >= DEV_BASE && addr < DEV_END && this.io)
      return this.io.read32(addr) | 0;
    if (!(addr & 3)) return this.i32[addr >> 2] | 0;
    if (addr + 4 > this.size) return 0;
    return this.dv.getInt32(addr, true);
  }

  write32(addr, val) {
    addr = addr >>> 0;
    if (addr >= DEV_BASE && addr < DEV_END && this.io)
      return this.io.write32(addr, val | 0);
    if (!(addr & 3)) { this.i32[addr >> 2] = val; return; }
    if (addr + 4 > this.size) return;
    this.dv.setInt32(addr, val, true);
  }

  read8s(addr) { return (this.u8[addr >>> 0] << 24) >> 24; }
  read8u(addr) { return this.u8[addr >>> 0]; }
  write8(addr, val) {
    addr = addr >>> 0;
    if (addr >= DEV_BASE && addr < DEV_END && this.io)
      return this.io.write32(addr & ~3, val & 0xff);
    this.u8[addr] = val;
  }

  // Helpers for the loader and devices (no device interception).
  writeBytes(addr, bytes) { this.u8.set(bytes, addr); }
  cstring(addr, max = 4096) {
    addr = addr >>> 0;
    let end = addr;
    while (end < addr + max && this.u8[end]) end++;
    // a copy: a browser decodes no view of a SharedArrayBuffer
    return new TextDecoder('latin1').decode(this.u8.slice(addr, end));
  }
}

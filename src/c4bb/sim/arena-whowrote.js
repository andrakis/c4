// arena-whowrote.js - the c4bb arena, with a memory of who wrote what.
//
// A FORK of arena.js, and a fork on purpose: this records one extra
// word on every store, and c4bb's ordinary runs must not pay for it.
// src/c4bb/tests/check-fork-arena.sh strips the blocks between the
// `//>>> whowrote` and `//<<< whowrote` markers below and demands that
// what is left is arena.js, byte for byte. arena.js itself is not
// touched at all -- not one character -- so the unguarded machine is
// exactly the machine it was.
//
// WHAT IT IS FOR. C4KE finds its damage late:
//
//   c4ke: task 33 () OVERRAN ITS STACK: 44 of 262144 bytes, guard broken
//   c4ke: the heap below 0x1da472c is no longer trustworthy
//
// 44 bytes of stack used and the guard word broken, so the write came
// from somewhere else entirely, some unknown time earlier. Every
// question about that write has been answerable except the only one
// that matters: WHO. This answers it. One Uint32Array, one word per
// arena word, holding the PC of the last instruction to store there.
//
// WHY NOT REGIONS, like c4mpg. Regions need to know where allocations
// begin and end, and on the board that knowledge lives inside the
// firmware's free list (src/c4bb/fw/fw.c). Teaching the simulator to
// read fw.c's internals coupling the JS side to a C data structure that
// is itself a suspect. Provenance needs none of that: it does not care
// what a block is, only who last touched it, and it cannot be wrong
// about a layout it never had to learn.
//
// COST. The table is the same size as the arena (one 32-bit word per
// 32-bit word), so `-m 32` costs 32MB more and `-m 128` costs 128MB
// more. That is the whole price, and it is only paid under --whowrote.
//
// THE PC IT RECORDS is r[R.PC] as of the current opcode's entry -- the
// value after the fetch, so it points just past the instruction word.
// Both engines share that register array (turbo keeps working copies
// in JS locals but loads them from r at routine entry), which is why
// neither engine needed forking and --step is covered for free.

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
//>>> whowrote
// The query port, in the free space above the device registers
// (devices.js stops at PIT_MS = 0x1a0). Write an address to ask;
// read the PC that last stored there, or 0 for "nobody yet".
// ANNOUNCED, NEVER PROBED: without --whowrote these are ordinary
// unclaimed device addresses and read back 0, which is also the
// answer meaning "no record" -- so a guest cannot tell the tool is
// absent by poking at it, and must not try.
export const WW_QUERY  = 0x1a4;   // w: the address to ask about
export const WW_ANSWER = 0x1a8;   // r: PC of the last writer, or 0
//<<< whowrote
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
// The entry the kernel's map has for user address x, or 0
export function kernelPte(i32, size, udir, x) {
  const w = pteWord(i32, size, udir, x);
  return w < 0 ? 0 : i32[w] | 0;
}
// ... and its bit for a page put out to swap (C4IX's PTE_SWAP): the slot
// is in the entry's upper 20 bits
export const PTE_SWAP = 0x400;
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
//>>> whowrote
    // One word per arena word. Uint32Array so it starts zeroed:
    // 0 means nobody has written there yet, which is a true and
    // useful answer rather than a missing one.
    this.writer = new Uint32Array(sizeBytes >> 2);
    this.wwQuery = 0;
    // Set by cli.js once the Machine exists. Reading the PC out of
    // the shared register array rather than taking it as an argument
    // is what keeps turbo.js and machine.js unforked.
    this.regs = null;
    this.pcIndex = 0;
//<<< whowrote
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
//>>> whowrote
    if (addr === WW_ANSWER) return this.writer[this.wwQuery >> 2] | 0;
//<<< whowrote
    if (addr >= DEV_BASE && addr < DEV_END && this.io)
      return this.io.read32(addr) | 0;
    if (!(addr & 3)) return this.i32[addr >> 2] | 0;
    if (addr + 4 > this.size) return 0;
    return this.dv.getInt32(addr, true);
  }

  write32(addr, val) {
    addr = addr >>> 0;
//>>> whowrote
    if (addr === WW_QUERY) { this.wwQuery = val >>> 0; return; }
//<<< whowrote
    if (addr >= DEV_BASE && addr < DEV_END && this.io)
      return this.io.write32(addr, val | 0);
//>>> whowrote
    this.note(addr, 4);
//<<< whowrote
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
//>>> whowrote
    this.note(addr, 1);
//<<< whowrote
    this.u8[addr] = val;
  }

//>>> whowrote
  // Stamp the writing PC over every arena word this store touches.
  // An unaligned 4-byte store spans two words and both get stamped,
  // because either of them is a true answer to "who wrote this".
  note(addr, len) {
    const pc = this.regs ? this.regs[this.pcIndex] >>> 0 : 0;
    this.writer[addr >> 2] = pc;
    if (len > 1) this.writer[(addr + len - 1) >> 2] = pc;
  }

//<<< whowrote
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

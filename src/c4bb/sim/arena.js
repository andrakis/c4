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
//   0x0200-0x034F  opcode-name ROM (66 entries x 5 bytes, c4m layout)
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

export class Arena {
  constructor(sizeBytes = 32 * 1024 * 1024) {
    this.size = sizeBytes;
    this.buf = new ArrayBuffer(sizeBytes);
    this.i32 = new Int32Array(this.buf);
    this.u8 = new Uint8Array(this.buf);
    this.dv = new DataView(this.buf);
    this.io = null;          // devices.js Devices instance
    this.stackTop = sizeBytes - ARGS_RESERVE;
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
    return new TextDecoder('latin1').decode(this.u8.subarray(addr, end));
  }
}

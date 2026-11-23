// blk.js - the machine's disks as sectors: block devices, a window of
// registers at 0x540 that the kernel alone reaches (docs/c4ix-ext2.md 1;
// c4pc's dev.c is the same in C). C4IX keeps its own filesystem, ext2,
// on one (src/c4ix/ext2.c).
//
//   0x540 BLK_COUNT    r: block devices fitted
//   0x544 BLK_SEL      r/w: the selected one, from 0
//   0x548 BLK_SECTORS  r: its 512-byte sectors
//   0x54c BLK_FLAGS    r: 1 read-only, 2 removable
//   0x550 BLK_LBA      w: the first sector of the next command
//   0x554 BLK_NSECT    w: how many (256 at most)
//   0x558 BLK_ADDR     w: where in memory (physical: the kernel's)
//   0x55c BLK_CMD      w: 1 read, 2 write, 3 flush; r: 0 done, -1 failed
//
// A command is done when the write to BLK_CMD returns. A disk is
// { sectors, flags, read(lba, n) -> Uint8Array, write(lba, bytes),
// flush() }: cli.js's --disk FILE is an image file read and written in
// place (cli.js fileDisk). Nothing here is Node's, so a page can fit one.

export const BLK_BASE = 0x540, BLK_END = 0x560;
const COUNT = 0x540, SEL = 0x544, SECTORS = 0x548, FLAGS = 0x54c, LBA = 0x550, NSECT = 0x554, ADDR = 0x558, CMD = 0x55c;
const MAX_RUN = 256;

export class BlockDevices {
  constructor(disks) {
    this.disks = disks;
    this.sel = 0; this.lba = 0; this.nsect = 0; this.addr = 0; this.result = 0;
    this.arena = null;                  // boot.js
  }
  get count() { return this.disks.length; }
  read32(a) {
    const d = this.disks[this.sel];
    switch (a) {
      case COUNT: return this.disks.length;
      case SEL: return this.sel;
      case SECTORS: return d ? d.sectors | 0 : 0;
      case FLAGS: return d ? d.flags | 0 : 0;
      case LBA: return this.lba | 0;
      case NSECT: return this.nsect | 0;
      case ADDR: return this.addr | 0;
      case CMD: return this.result | 0;
    }
    return 0;
  }
  write32(a, v) {
    switch (a) {
      case SEL: this.sel = v | 0; return;
      case LBA: this.lba = v >>> 0; return;
      case NSECT: this.nsect = v >>> 0; return;
      case ADDR: this.addr = v >>> 0; return;
      case CMD: break;
      default: return;
    }
    const d = this.disks[this.sel];
    this.result = -1;
    if (!d) return;
    try {
      if (v === 3) { if (d.flush) d.flush(); this.result = 0; return; }
      if (v !== 1 && v !== 2) return;
      const n = this.nsect, lba = this.lba, at = this.addr, u8 = this.arena.u8;
      if (!n || n > MAX_RUN || lba >= d.sectors || n > d.sectors - lba) return;
      if (at + n * 512 > u8.length) return;
      if (v === 1) u8.set(d.read(lba, n), at);
      else {
        if (d.flags & 1) return;
        d.write(lba, u8.slice(at, at + n * 512));
      }
      this.result = 0;
    } catch { this.result = -1; }
  }
}

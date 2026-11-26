// bus.js - everything below MEM_BASE (0x1000).
//
// The interpreter reads and writes ordinary memory straight through the
// arena's typed arrays and comes here only for low addresses, which is
// where the machine's hardware lives:
//
//   0x0100-0x01FF  c4bb's device registers (src/c4bb/sim/devices.js),
//                  reached through Arena.read32/write32 as on c4bb
//   0x0400-0x047F  the display (gui-device.js), when one is fitted
//   0x0480-0x04FF  the sound card (snd-device.js), when one is fitted
//   0x0540-0x055F  the block devices (blk.js), when any are fitted
//   0x0580-0x059F  the network card (net.js), when one is fitted
//   everything else  RAM: the TLEV word, the opcode-name ROM, ...
//
// Unaligned and out-of-range accesses behave as they do on c4bb: reads
// give 0, writes are dropped.

export const GUI_BASE = 0x400, GUI_END = 0x480, SND_BASE = 0x480, SND_END = 0x500;
export const BLK_BASE = 0x540, BLK_END = 0x560, NET_BASE = 0x580, NET_END = 0x5a0;

export class Bus {
  constructor(arena, gui = null, snd = null, blk = null, net = null) {
    this.arena = arena;
    this.gui = gui;
    this.snd = snd;
    this.blk = blk && blk.count ? blk : null;
    if (this.blk) this.blk.arena = arena;
    this.net = net;
    if (net) net.arena = arena;
  }

  read32(addr) {
    addr = addr >>> 0;
    if (addr < 0x10) return 0;           // the null guard reads 0 (c4m.js NULL_GUARD_OP)
    if (addr >= GUI_BASE && addr < GUI_END && this.gui) return this.gui.read32(addr & ~3) | 0;
    if (addr >= SND_BASE && addr < SND_END && this.snd) return this.snd.read32(addr & ~3) | 0;
    if (addr >= BLK_BASE && addr < BLK_END && this.blk) return this.blk.read32(addr & ~3) | 0;
    if (addr >= NET_BASE && addr < NET_END && this.net) return this.net.read32(addr & ~3) | 0;
    return this.arena.read32(addr) | 0;
  }

  write32(addr, val) {
    addr = addr >>> 0;
    if (addr >= GUI_BASE && addr < GUI_END && this.gui) { this.gui.write32(addr & ~3, val | 0); return; }
    if (addr >= SND_BASE && addr < SND_END && this.snd) { this.snd.write32(addr & ~3, val | 0); return; }
    if (addr >= BLK_BASE && addr < BLK_END && this.blk) { this.blk.write32(addr & ~3, val | 0); return; }
    if (addr >= NET_BASE && addr < NET_END && this.net) { this.net.write32(addr & ~3, val | 0); return; }
    this.arena.write32(addr, val | 0);
  }

  read8s(addr) {
    if ((addr >>> 0) < 0x10) return 0;
    return this.arena.read8s(addr);
  }

  write8(addr, val) {
    addr = addr >>> 0;
    if (addr >= GUI_BASE && addr < GUI_END && this.gui) { this.gui.write32(addr & ~3, val & 0xff); return; }
    if (addr >= SND_BASE && addr < SND_END && this.snd) { this.snd.write32(addr & ~3, val & 0xff); return; }
    this.arena.write8(addr, val);
  }
}

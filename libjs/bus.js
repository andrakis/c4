// bus.js - everything below MEM_BASE (0x1000).
//
// The interpreter reads and writes ordinary memory straight through the
// arena's typed arrays and comes here only for low addresses, which is
// where the machine's hardware lives:
//
//   0x0100-0x01FF  c4bb's device registers (src/c4bb/sim/devices.js),
//                  reached through Arena.read32/write32 as on c4bb
//   0x0400-0x047F  the display (gui-device.js), when one is fitted
//   everything else  RAM: the TLEV word, the opcode-name ROM, ...
//
// Unaligned and out-of-range accesses behave as they do on c4bb: reads
// give 0, writes are dropped.

export const GUI_BASE = 0x400, GUI_END = 0x480;

export class Bus {
  constructor(arena, gui = null) {
    this.arena = arena;
    this.gui = gui;
  }

  read32(addr) {
    addr = addr >>> 0;
    if (addr >= GUI_BASE && addr < GUI_END && this.gui) return this.gui.read32(addr & ~3) | 0;
    return this.arena.read32(addr) | 0;
  }

  write32(addr, val) {
    addr = addr >>> 0;
    if (addr >= GUI_BASE && addr < GUI_END && this.gui) { this.gui.write32(addr & ~3, val | 0); return; }
    this.arena.write32(addr, val | 0);
  }

  read8s(addr) {
    return this.arena.read8s(addr);
  }

  write8(addr, val) {
    addr = addr >>> 0;
    if (addr >= GUI_BASE && addr < GUI_END && this.gui) { this.gui.write32(addr & ~3, val & 0xff); return; }
    this.arena.write8(addr, val);
  }
}

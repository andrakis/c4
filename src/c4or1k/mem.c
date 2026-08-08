#include "mem.h"
#include "mmio.h"

char *ram;

void mem_init() {
    ram = malloc(RAM_SIZE);
    memset(ram, 0, RAM_SIZE);
}

// RAM vs MMIO is decided by bit 31 of the (already physical, past any
// DTLB translation) address -- jor1k's Read/WriteXBig do this as
// `addr >= 0` on a genuine JS int32, which is bit 31 in disguise.
// c4lc's `int` doesn't wrap at 32 bits, so an address a register
// happens to hold sign-extended (e.g. the UART base 0x90000000,
// stored via sext(..., 32) because it doesn't fit as a positive
// 32-bit value) can arrive here as a large *negative* 64-bit number
// instead of a small positive one -- "addr < 0" would still work by
// accident for that specific case, but `addr & 0x80000000` is the
// actual, width-independent test, and is what MMIO_BIT below uses.
enum { MMIO_BIT = 0x80000000 };

int ram_lw(int addr) {
    int a, b0, b1, b2, b3;
    if (addr & MMIO_BIT) return mmio_read32(addr);
    a = addr & (RAM_SIZE - 1);
    b0 = ram[a] & 0xFF;
    b1 = ram[a + 1] & 0xFF;
    b2 = ram[a + 2] & 0xFF;
    b3 = ram[a + 3] & 0xFF;
    return (b0 << 24) | (b1 << 16) | (b2 << 8) | b3;
}

// The MMIO case composes from two 8-bit reads rather than a dedicated
// mmio_read16 -- see docs/c4or1k-design.md's M5 section: a genuine
// mmio_read16/virtio_read16 pair reproducibly crashed c4m itself deep
// inside a real Linux boot (a wild pointer dereference in c4m's own
// interpreter, unrelated to any actual out-of-bounds c4or1k access as
// far as could be determined), while this byte-composed form -- built
// entirely from the already-proven-correct 8-bit path -- does not.
int ram_lh(int addr) {
    int a, b0, b1;
    if (addr & MMIO_BIT) return (mmio_read8(addr) << 8) | mmio_read8(addr + 1);
    a = addr & (RAM_SIZE - 1);
    b0 = ram[a] & 0xFF;
    b1 = ram[a + 1] & 0xFF;
    return (b0 << 8) | b1;
}

int ram_lb(int addr) {
    if (addr & MMIO_BIT) return mmio_read8(addr);
    return ram[addr & (RAM_SIZE - 1)] & 0xFF;
}

void ram_sw(int addr, int val) {
    int a;
    if (addr & MMIO_BIT) { mmio_write32(addr, val); return; }
    a = addr & (RAM_SIZE - 1);
    ram[a] = (val >> 24) & 0xFF;
    ram[a + 1] = (val >> 16) & 0xFF;
    ram[a + 2] = (val >> 8) & 0xFF;
    ram[a + 3] = val & 0xFF;
}

void ram_sh(int addr, int val) {
    int a;
    if (addr & MMIO_BIT) { mmio_write8(addr, (val >> 8) & 0xFF); mmio_write8(addr + 1, val & 0xFF); return; }
    a = addr & (RAM_SIZE - 1);
    ram[a] = (val >> 8) & 0xFF;
    ram[a + 1] = val & 0xFF;
}

void ram_sb(int addr, int val) {
    if (addr & MMIO_BIT) { mmio_write8(addr, val); return; }
    ram[addr & (RAM_SIZE - 1)] = val & 0xFF;
}

// Prints nwords words starting at base, one per line, for diffing
// against tools/or1k-oracle.js's identical dump of jor1k's ram.
void ram_dump(int base, int nwords) {
    int i;
    for (i = 0; i < nwords; ++i) {
        printf("m%d=%d\n", i, ram_lw(base + i * 4));
    }
}

#include "mem.h"

char *ram;

void mem_init() {
    ram = malloc(RAM_SIZE);
    memset(ram, 0, RAM_SIZE);
}

int ram_lw(int addr) {
    int a, b0, b1, b2, b3;
    a = addr & (RAM_SIZE - 1);
    b0 = ram[a] & 0xFF;
    b1 = ram[a + 1] & 0xFF;
    b2 = ram[a + 2] & 0xFF;
    b3 = ram[a + 3] & 0xFF;
    return (b0 << 24) | (b1 << 16) | (b2 << 8) | b3;
}

int ram_lh(int addr) {
    int a, b0, b1;
    a = addr & (RAM_SIZE - 1);
    b0 = ram[a] & 0xFF;
    b1 = ram[a + 1] & 0xFF;
    return (b0 << 8) | b1;
}

int ram_lb(int addr) {
    return ram[addr & (RAM_SIZE - 1)] & 0xFF;
}

void ram_sw(int addr, int val) {
    int a;
    a = addr & (RAM_SIZE - 1);
    ram[a] = (val >> 24) & 0xFF;
    ram[a + 1] = (val >> 16) & 0xFF;
    ram[a + 2] = (val >> 8) & 0xFF;
    ram[a + 3] = val & 0xFF;
}

void ram_sh(int addr, int val) {
    int a;
    a = addr & (RAM_SIZE - 1);
    ram[a] = (val >> 8) & 0xFF;
    ram[a + 1] = val & 0xFF;
}

void ram_sb(int addr, int val) {
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

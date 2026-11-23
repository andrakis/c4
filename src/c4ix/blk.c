// blk.c -- the machine's disks, as sectors (docs/c4ix-ext2.md 1): the
// window of registers at 0x540 that libjs/blk.js and c4pc's dev.c keep
// (c4pc: the PC's IDE disks; both: --disk FILE.img). Kernel mode's alone:
// a user task's IOPM has no bit for it. ext2.c keeps its filesystem here.
//
// A command moves up to 256 sectors between a disk and memory (the
// kernel's, physical) and is done when the write that starts it returns.

#include "c4ix.h"

enum {
    BLK_COUNT = 0x540, BLK_SEL = 0x544, BLK_SECTORS = 0x548, BLK_FLAGS = 0x54c,
    BLK_LBA = 0x550, BLK_NSECT = 0x554, BLK_ADDR = 0x558, BLK_CMD = 0x55c,
    BLK_RUN = 256
};

static int blk_lk[2];          // the registers: one command at a time, whichever CPU

int blk_count() {
    if (!host_has(C4IX_I_BLK)) return 0;
    return *(int *)BLK_COUNT;
}
int blk_sectors(int dev) {
    int n;
    if (dev < 0 || dev >= blk_count()) return 0;
    klock_take(blk_lk);
    *(int *)BLK_SEL = dev;
    n = *(int *)BLK_SECTORS;
    klock_drop(blk_lk);
    return n;
}
int blk_flags(int dev) {
    int f;
    if (dev < 0 || dev >= blk_count()) return 0;
    klock_take(blk_lk);
    *(int *)BLK_SEL = dev;
    f = *(int *)BLK_FLAGS;
    klock_drop(blk_lk);
    return f;
}

// N sectors at LBA of DEV, into BUF (CMD 1) or from it (CMD 2): 0, or -1
static int blk_io(int dev, int lba, int n, char *buf, int cmd) {
    int k, r;
    if (dev < 0 || dev >= blk_count() || n < 0) return 0 - 1;
    r = 0;
    while (n > 0 && !r) {
        k = n;
        if (k > BLK_RUN) k = BLK_RUN;
        klock_take(blk_lk);
        *(int *)BLK_SEL = dev;
        *(int *)BLK_LBA = lba;
        *(int *)BLK_NSECT = k;
        *(int *)BLK_ADDR = (int)buf;
        *(int *)BLK_CMD = cmd;
        r = *(int *)BLK_CMD;
        klock_drop(blk_lk);
        lba = lba + k;
        n = n - k;
        buf = buf + k * 512;
    }
    return r ? 0 - 1 : 0;
}
int blk_read(int dev, int lba, int n, char *buf) { return blk_io(dev, lba, n, buf, 1); }
int blk_write(int dev, int lba, int n, char *buf) { return blk_io(dev, lba, n, buf, 2); }
int blk_flush(int dev) {
    int r;
    if (dev < 0 || dev >= blk_count()) return 0 - 1;
    klock_take(blk_lk);
    *(int *)BLK_SEL = dev;
    *(int *)BLK_CMD = 3;
    r = *(int *)BLK_CMD;
    klock_drop(blk_lk);
    return r;
}

// the boot's account: each disk, a line
void blk_banner() {
    int n, k, s;
    n = blk_count();
    k = 0;
    while (k < n) {
        s = blk_sectors(k);
        klog("blk: hd%d: %d MB%s\n", k, s / 2048, (blk_flags(k) & 1) ? ", read-only" : "");
        ++k;
    }
}

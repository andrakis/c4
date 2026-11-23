// C4IX userland: mkfs -- an empty ext2 filesystem on a disk.
//
//   mkfs [-b 1024|2048|4096] [-L LABEL] hdN    (or a partition: hdNpP)
//
// The kernel makes it (ext2.c ext2_mkfs; docs/c4ix-ext2.md): its block
// groups, bitmaps and inode tables, a root directory and lost+found. What
// was on the disk is gone. Blocks of 1 KB below 512 MB, 4 KB from there,
// unless -b says. Then `mount hdN DIR`. Linux's e2fsck, debugfs and a loop
// mount all read what it makes.

#include "c4ix_user.h"

enum { SYS_MKFS = 250 };

static int ksys(int num, int a, int b, int c) { return __c4_opcode(c, b, a, num); }

static int num(char *s) {
    int v;
    v = 0;
    while (*s >= '0' && *s <= '9') { v = v * 10 + *s - '0'; ++s; }
    return v;
}

// a disk or partition by name, hdD or hdDpP (blk.c's numbers): -1 if not one
static int disk_id(char *s) {
    if (s[0] != 'h' || s[1] != 'd' || s[2] < '0' || s[2] > '7') return 0 - 1;
    if (!s[3]) return s[2] - '0';
    if (s[3] != 'p' || s[4] < '1' || s[4] > '4' || s[5]) return 0 - 1;
    return 8 + (s[2] - '0') * 4 + s[4] - '1';
}

int main(int argc, char **argv) {
    char *label, *dev;
    int a, bsize, d;
    bsize = 0; label = 0; dev = 0;
    a = 1;
    while (a < argc) {
        if (!ustrcmp(argv[a], "-b") && a + 1 < argc) { ++a; bsize = num(argv[a]); }
        else if (!ustrcmp(argv[a], "-L") && a + 1 < argc) { ++a; label = argv[a]; }
        else if (!dev) dev = argv[a];
        else { dev = 0; break; }
        ++a;
    }
    if (!dev || (d = disk_id(dev)) < 0) {
        ufprintf(STDERR, "usage: mkfs [-b 1024|2048|4096] [-L LABEL] hdN | hdNpP   (cat /proc/disks lists them)\n");
        return 1;
    }
    if (bsize && bsize != 1024 && bsize != 2048 && bsize != 4096) { ufprintf(STDERR, "mkfs: blocks of 1024, 2048 or 4096 bytes\n"); return 1; }
    uprintf("mkfs: making an ext2 filesystem on %s: what is on it now is gone\n", dev);
    if (ksys(SYS_MKFS, d, bsize, (int)label) < 0) {
        ufprintf(STDERR, "mkfs: %s could not be made one (dmesg says why)\n", dev);
        return 1;
    }
    uprintf("mkfs: done: mount %s DIR\n", dev);
    return 0;
}

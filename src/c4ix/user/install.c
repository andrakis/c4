// C4IX userland: install -- C4IX onto a PC's disk, to boot from it.
//
//   install [-y] [hdN]    hd0 by default; -y does not ask first
//
// What is on the disk now is ERASED: install says so, with the disk's
// size, and goes on only when "yes" is typed. Then the disk holds the
// system that is running -- the 32-bit one or the 64-bit one, whichever
// the boot disc's menu started -- laid out to boot without the disc
// (docs/c4ix-install.md):
//
//   sector 0        GRUB's boot.img and a partition table
//   1 to 2047       GRUB's core.img (ext2, the MBR's partitions and
//                   multiboot in it; its grub.cfg on partition 1)
//   hdNp1, 0x83     ext2, "C4IX": the whole of /, as it is now (every
//                   directory but /proc, /tmp and /mnt), and /boot: c4pc
//                   and the kernel, and grub/grub.cfg. Booted with
//                   --root hdNp1: nothing unpacked, every change kept.
//
// c4pc and the kernel come from c4pc's boot disc (the CD with volume id
// C4PC), mounted at /mnt/c4pc if it is not already: the files its menu
// for a disk names, /boot/grub/disk.cfg (disk64.cfg for the 64-bit
// system), which install copies as grub.cfg with @ROOT@ the partition.
// GRUB's two images are its /boot/grub/c4pc-boot.img and c4pc-core.img
// (the ISO's build makes them: Makefile, c4pc.iso).

#include "c4ix_user.h"

enum { C4I_MMU = 0x80000, DISK_COUNT = 0x188 };
enum { SYS_MOUNT = 248, SYS_UMOUNT = 249, SYS_MKFS = 250, SYS_SYNC = 252, SYS_CHMOD = 253, SYS_BLKIO = 255 };
enum { RUN = 64, BUFSZ = 32768, CORE_MAX = 2047 * 512, PATHW = 256, START = 2048, NAMEW = 64 };

static char buf[BUFSZ];
static char core[CORE_MAX];
static char text[16384];
static char line[PATHW];
static char src[PATHW];
static int n_files, n_dirs, n_bad, kb_done, kb_all, kb_said;

static int ksys(int num, int a, int b, int c) { return __c4_opcode(c, b, a, num); }
// COUNT sectors (to 64) at LBA of device DEV, from (WRITE) or into BUF
static int blkio(int dev, int lba, int count, char *b, int write) {
    return ksys(SYS_BLKIO, dev + (write ? 256 : 0) + (count - 1) * 65536, lba, (int)b);
}
static void put32(char *p, int v) { p[0] = v & 255; p[1] = (v >> 8) & 255; p[2] = (v >> 16) & 255; p[3] = (v >> 24) & 255; }
// DIR/NAME into TO (a path's room): TO
static char *join(char *to, char *dir, char *name) { upathjoin(to, PATHW, dir, name); return to; }

// a disk's sectors and read-only flag, from /proc/disks ("hd0 SECTORS rw"):
// the sectors, or -1 if there is no such disk
static int disk_size(char *name, int *ro) {
    char *p, *w;
    int n;
    if (uslurp("/proc/disks", text, 16384) < 0) return 0 - 1;
    p = text;
    while (*p) {
        w = p; while (*p && *p != ' ') ++p; if (*p) { *p = 0; ++p; }
        n = uatoi(p);
        while (*p && *p != ' ') ++p; if (*p) ++p;
        *ro = *p == 'r' && p[1] == 'o';
        while (*p && *p != 10) ++p; if (*p) ++p;
        if (!ustrcmp(w, name)) return n;
    }
    return 0 - 1;
}
// is anything of disk NAME mounted (/proc/mounts: "hd0p1 /home ext2 rw")
static int disk_busy(char *name) {
    char *p, *w;
    int k;
    if (uslurp("/proc/mounts", text, 16384) < 0) return 0;
    p = text;
    while (*p) {
        w = p;
        k = 0;
        while (name[k] && w[k] == name[k]) ++k;
        if (!name[k] && (w[k] == ' ' || w[k] == 'p')) return 1;
        while (*p && *p != 10) ++p; if (*p) ++p;
    }
    return 0;
}
// is the line at P (to a newline or the end) WANT
static int line_is(char *p, char *want) {
    while (*want && *p == *want) { ++p; ++want; }
    return !*want && (*p == 10 || *p == 13 || !*p);
}
static int is_file(char *path, int *size) {
    int st[4];
    if (ustat(path, st) < 0 || st[0] != 2) return 0;
    *size = st[1];
    return 1;
}
// c4pc's boot disc at /mnt/c4pc: there already, or mounted now (its drive
// is the CD whose label is C4PC: N:.c4pc-drive). 1 if it is there
static int boot_disc() {
    char name[16], *av[4], *p;
    int n, k, id, size;
    if (is_file("/mnt/c4pc/boot/grub/c4pc-core.img", &size)) return 1;
    if (is_file("/mnt/cdrom/boot/grub/c4pc-core.img", &size)) { ustrcpy(src, "/mnt/cdrom"); return 1; }
    n = *(int *)DISK_COUNT;
    k = 2;
    while (k < n && k < 10) {
        name[0] = '0' + k; name[1] = ':'; ustrcpy(name + 2, ".c4pc-drive");
        if (uslurp(name, text, 4096) >= 0) {
            p = text;
            while (*p) {
                if (line_is(p, "label C4PC")) {
                    name[0] = '0' + k; name[1] = 0;
                    av[0] = "mount"; av[1] = name; av[2] = "/mnt/c4pc"; av[3] = 0;
                    if ((id = spawn("/bin/mount", 3, av)) < 0 || uwait(id) != 0) return 0;
                    return is_file("/mnt/c4pc/boot/grub/c4pc-core.img", &size);
                }
                while (*p && *p != 10) ++p; if (*p) ++p;
            }
        }
        ++k;
    }
    return 0;
}
// a file of the boot disc's /boot into TO: its size, or -1
static int read_all(char *name, char *to, int max) {
    int fd, got, r;
    ustrcpy(line, src);
    ustrcat(line, "/boot/");
    ustrcat(line, name);
    if ((fd = uopen(line, O_RD)) < 0) { ufprintf(STDERR, "install: %s is not on the boot disc\n", line); return 0 - 1; }
    got = 0;
    while (got < max && (r = uread(fd, to + got, max - got)) > 0) got = got + r;
    uclose(fd);
    return got;
}
// FROM to TO, a file, its bytes as they are; a program's mode 0755. 0, or
// -1. (A big one goes to the disk as it is written, past the first MB,
// not into RAM: vfs.c, e2_spill.)
static int copy_one(char *from, char *to, int prog) {
    int in, out, r, total;
    if ((in = uopen(from, O_RD)) < 0) { ufprintf(STDERR, "install: %s cannot be read\n", from); return 0 - 1; }
    if ((out = uopen(to, O_WR + O_CREATE + O_TRUNCATE)) < 0) { uclose(in); ufprintf(STDERR, "install: %s cannot be made\n", to); return 0 - 1; }
    total = 0;
    while ((r = uread(in, buf, BUFSZ)) > 0) {
        if (write(out, buf, r) != r) { uclose(in); uclose(out); ufprintf(STDERR, "install: %s: a write failed (the disk full?)\n", to); return 0 - 1; }
        total = total + r;
    }
    uclose(in);
    if (r < 0) { uclose(out); ufprintf(STDERR, "install: %s could not be read to its end\n", from); return 0 - 1; }
    if (uclose(out) < 0) { ufprintf(STDERR, "install: %s could not be written to the disk\n", to); return 0 - 1; }
    if (prog) ksys(SYS_CHMOD, (int)to, 493, 0);          // 0755
    kb_done = kb_done + (total + 1023) / 1024;
    return 0;
}
// is NAME at the top one install leaves: the kernel's (/proc), the
// session's (/tmp), and where things are mounted (/mnt: this disk, the CD)
static int skip_top(char *name) {
    return !ustrcmp(name, "proc") || !ustrcmp(name, "tmp") || !ustrcmp(name, "mnt") || !ustrcmp(name, "dev");
}
// the KB under DIR, and how many files (the size install will copy)
static int size_tree(char *dir, int top, int *files) {
    char name[NAMEW], sub[PATHW];
    int i, k, kb, st[4];
    kb = 0;
    i = 0;
    while ((k = ureaddir(dir, i, name)) >= 0) {
        ++i;
        if (top && skip_top(name)) continue;
        if (ustrlen(dir) + ustrlen(name) + 2 >= PATHW) continue;
        join(sub, dir, name);
        if (k == 1) kb = kb + 1 + size_tree(sub, 0, files);
        else if (ustat(sub, st) >= 0) { kb = kb + (st[1] + 1023) / 1024 + 1; *files = *files + 1; }
    }
    return kb;
}
// everything under FROM made again under TO (made already)
static int copy_tree(char *from, char *to, int top) {
    char name[NAMEW], a[PATHW], b[PATHW];
    int i, k, st[4];
    i = 0;
    while ((k = ureaddir(from, i, name)) >= 0) {
        ++i;
        if (top && skip_top(name)) continue;
        if (ustrlen(from) + ustrlen(name) + 2 >= PATHW || ustrlen(to) + ustrlen(name) + 2 >= PATHW) {
            ufprintf(STDERR, "install: %s/%s: the path is too long; left out\n", from, name); ++n_bad; continue;
        }
        join(a, from, name);
        join(b, to, name);
        if (k == 1) {
            if (umkdir(b) < 0 && ustat(b, st) < 0) { ufprintf(STDERR, "install: %s cannot be made\n", b); return 0 - 1; }
            ++n_dirs;
            if (copy_tree(a, b, 0) < 0) return 0 - 1;
            if (top) uprintf("install:   /%s\n", name);
        } else {
            if (ustat(a, st) < 0) { ++n_bad; continue; }
            if (copy_one(a, b, st[2]) < 0) { if (kb_done + 1024 > kb_all) return 0 - 1; ++n_bad; continue; }
            ++n_files;
            if (kb_all > 0 && kb_done - kb_said >= 16384) {
                kb_said = kb_done;
                uprintf("install:   %d of %d MB\n", kb_done / 1024, kb_all / 1024);
            }
        }
    }
    return 0;
}
// the root archive (c4ix-root.tgz: the sources, the libraries) is still
// being unpacked, in the background, when a console's boot is done:
// wait for it, or the disk would get half the tree
static void wait_unpack() {
    int info[TASKINFO_WORDS];
    int i, busy, said;
    said = 0;
    for (;;) {
        busy = 0;
        i = 0;
        while (utaskinfo(i, info)) {
            if (info[2] != TS_ZOMBIE && !ustrncmp((char *)(info + 8), "c4ix-persist", 12)) busy = 1;
            ++i;
        }
        if (!busy) return;
        if (!said) { uprintf("install: waiting for the system's files to finish unpacking\n"); said = 1; }
        umsleep(500);
    }
}
// the boot disc's /boot files the menu names (multiboot /boot/X, module
// /boot/Y), to the new disk's /boot. 0, or -1
static int copy_boot(char *cfg) {
    char name[NAMEW], from[PATHW], to[PATHW];
    char *p;
    int k, at;
    p = cfg;
    while (*p) {
        while (*p == ' ' || *p == 9) ++p;
        at = 0;
        if (!ustrncmp(p, "multiboot /boot/", 16)) at = 16;
        else if (!ustrncmp(p, "module /boot/", 13)) at = 13;
        if (at) {
            p = p + at;
            k = 0;
            while (*p && *p != ' ' && *p != 10 && k < NAMEW - 1) { name[k] = *p; ++k; ++p; }
            name[k] = 0;
            join(to, "/mnt/install/boot", name);
            if (!is_file(to, &k)) {
                join(from, src, "boot");
                join(from, from, name);
                if (copy_one(from, to, 0) < 0) return 0 - 1;
                uprintf("install:   /boot/%s\n", name);
            }
        }
        while (*p && *p != 10) ++p;
        if (*p) ++p;
    }
    return 0;
}

int main(int argc, char **argv) {
    char *disk, *p, *menu;
    char dev1[8];
    int k, yes, d, sectors, ro, p1, n, fd, lba, coresz, cfgn, i, files;
    if (__c4_info() & C4I_MMU) uioperm(UIOPERM_DEV);
    yes = 0; disk = "hd0";
    k = 1;
    while (k < argc) {
        if (!ustrcmp(argv[k], "-y")) yes = 1;
        else if (argv[k][0] == 'h' && argv[k][1] == 'd' && argv[k][2] >= '0' && argv[k][2] <= '7' && !argv[k][3]) disk = argv[k];
        else { ufprintf(STDERR, "usage: install [-y] [hdN]   (hd0 by default: cat /proc/disks)\n"); return 1; }
        ++k;
    }
    d = disk[2] - '0';
    // the menu for the system that is running: the word's
    menu = sizeof(int) == 8 ? "grub/disk64.cfg" : "grub/disk.cfg";
    if ((sectors = disk_size(disk, &ro)) < 0) { ufprintf(STDERR, "install: there is no disk %s (cat /proc/disks lists them)\n", disk); return 1; }
    if (ro) { ufprintf(STDERR, "install: %s is read-only\n", disk); return 1; }
    if (disk_busy(disk)) { ufprintf(STDERR, "install: %s, or a partition of it, is mounted: umount it first\n", disk); return 1; }
    ustrcpy(src, "/mnt/c4pc");
    if (!boot_disc()) { ufprintf(STDERR, "install: c4pc's boot disc (the CD it booted from) is not in a drive: boot from it to install\n"); return 1; }
    if ((cfgn = read_all(menu, text, 12000)) <= 0) { ufprintf(STDERR, "install: the boot disc has no %s: this system cannot be installed from it\n", menu); return 1; }
    wait_unpack();
    files = 0;
    kb_all = size_tree("/", 1, &files) + 8192;            // and c4pc, the kernel
    p1 = (sectors - START) / 2048 * 2048;
    if (p1 / 2 < kb_all + kb_all / 8 + 16384) {
        ufprintf(STDERR, "install: %s is %d MB: too small for the system's %d MB (and room for an ext2's own blocks and yours)\n",
            disk, sectors / 2048, kb_all / 1024);
        return 1;
    }
    uprintf("\n");
    uprintf("  install: C4IX (%d-bit) onto %s, a disk of %d MB.\n", sizeof(int) * 8, disk, sectors / 2048);
    uprintf("\n");
    uprintf("  WARNING: EVERYTHING ON %s WILL BE ERASED: its partitions, its files,\n", disk);
    uprintf("  whatever system was on it. This cannot be undone.\n");
    uprintf("\n");
    uprintf("  It becomes GRUB to boot from and one ext2 partition, %d MB, holding\n", p1 / 2048);
    uprintf("  the whole system (%d files, %d MB) and your files.\n", files, kb_all / 1024);
    uprintf("\n");
    if (!yes) {
        uprintf("  Type yes to erase %s and install, anything else to stop: ", disk);
        n = uread(0, line, PATHW - 1);
        if (n < 0) n = 0;
        line[n] = 0;
        p = line;
        while (*p == ' ') ++p;
        if (!(p[0] == 'y' && p[1] == 'e' && p[2] == 's' && (p[3] == 10 || p[3] == 13 || p[3] == ' ' || !p[3]))) {
            uprintf("install: stopped; %s is as it was\n", disk);
            return 1;
        }
    }
    // GRUB: core.img into the sectors before the first partition, then
    // boot.img and the partition table in sector 0
    if ((coresz = read_all("grub/c4pc-core.img", core, CORE_MAX)) <= 512) return 1;
    if (coresz == CORE_MAX) { ufprintf(STDERR, "install: GRUB's core.img is too big to fit before sector 2048\n"); return 1; }
    if (read_all("grub/c4pc-boot.img", buf, 512) != 512) { ufprintf(STDERR, "install: GRUB's boot.img is not 512 bytes\n"); return 1; }
    uprintf("install: GRUB, %d KB, into %s's first sectors\n", (coresz + 1023) / 1024, disk);
    n = (coresz + 511) / 512;
    i = coresz;
    while (i < n * 512) { core[i] = 0; ++i; }
    lba = 0;
    while (lba < n) {
        k = n - lba;
        if (k > RUN) k = RUN;
        if (blkio(d, 1 + lba, k, core + lba * 512, 1) < 0) { ufprintf(STDERR, "install: %s could not be written\n", disk); return 1; }
        lba = lba + k;
    }
    // the MBR: boot.img's code (440 bytes), a disk signature, the table:
    // partition 1, active, ext2 (0x83), from 2048 to the end
    i = 440;
    while (i < 510) { buf[i] = 0; ++i; }
    put32(buf + 440, ucycles() | 1);
    p = buf + 446;
    p[0] = 0x80; p[1] = 0xFE; p[2] = 0xFF; p[3] = 0xFF; p[4] = 0x83; p[5] = 0xFE; p[6] = 0xFF; p[7] = 0xFF;
    put32(p + 8, START); put32(p + 12, p1);
    buf[510] = 0x55; buf[511] = 0xAA;
    if (blkio(d, 0, 1, buf, 1) < 0) { ufprintf(STDERR, "install: %s's first sector could not be written\n", disk); return 1; }
    ustrcpy(dev1, disk); ustrcpy(dev1 + 3, "p1");
    uprintf("install: an ext2 filesystem on %s\n", dev1);
    if (ksys(SYS_MKFS, 8 + d * 4, 0, (int)"C4IX") < 0) { ufprintf(STDERR, "install: %s could not be made ext2 (dmesg says why)\n", dev1); return 1; }
    umkdir("/mnt");
    umkdir("/mnt/install");
    if (ksys(SYS_MOUNT, 8 + d * 4, (int)"/mnt/install", 0) < 0) { ufprintf(STDERR, "install: %s could not be mounted at /mnt/install (dmesg says why)\n", dev1); return 1; }
    // the system: / as it is, but /proc, /tmp and /mnt
    uprintf("install: the system, %d MB in %d files, onto %s\n", kb_all / 1024, files, dev1);
    if (copy_tree("/", "/mnt/install", 1) < 0) { ufprintf(STDERR, "install: stopped: %s is not a system that boots\n", disk); return 1; }
    umkdir("/mnt/install/tmp");
    umkdir("/mnt/install/mnt");
    // c4pc and the kernel, and the menu: the boot disc's for a disk, @ROOT@
    // the partition
    umkdir("/mnt/install/boot");
    umkdir("/mnt/install/boot/grub");
    uprintf("install: c4pc and the kernel into /boot\n");
    text[cfgn] = 0;
    if (copy_boot(text) < 0) return 1;
    if ((fd = uopen("/mnt/install/boot/grub/grub.cfg", O_WR + O_CREATE + O_TRUNCATE)) < 0) { ufprintf(STDERR, "install: grub.cfg cannot be made\n"); return 1; }
    i = 0;
    while (text[i]) {
        if (!ustrncmp(text + i, "@ROOT@", 6)) { write(fd, dev1, ustrlen(dev1)); i = i + 6; }
        else { k = i; while (text[k] && text[k] != '@') ++k; if (k == i) k = i + 1; write(fd, text + i, k - i); i = k; }
    }
    uclose(fd);
    uprintf("install:   /boot/grub/grub.cfg\n");
    ksys(SYS_SYNC, 0, 0, 0);
    if (ksys(SYS_UMOUNT, (int)"/mnt/install", 0, 0) < 0) ufprintf(STDERR, "install: /mnt/install could not be unmounted: it is written, but shut down before you boot from it\n");
    uprintf("\n");
    uprintf("  install: done: %d files, %d directories, %d MB", n_files, n_dirs, kb_done / 1024);
    if (n_bad) uprintf(" (%d left out: above)", n_bad);
    uprintf(".\n  C4IX boots from %s now: take the CD out (or boot from the disk first,\n", disk);
    uprintf("  the VM's BIOS settings) and reboot. / is %s's: every change is kept.\n", dev1);
    uprintf("\n");
    return 0;
}

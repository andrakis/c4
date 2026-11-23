// C4IX userland: install -- C4IX onto a PC's disk, to boot from it.
//
//   install [-y] [hdN]    hd0 by default; -y does not ask first
//
// What is on the disk now is ERASED: install says so, with the disk's
// size, and goes on only when "yes" is typed. Then the disk is c4pc's
// boot disc's, laid out to boot without it (docs/c4ix-install.md):
//
//   sector 0        GRUB's boot.img and a partition table
//   1 to 2047       GRUB's core.img (ext2, the MBR's partitions and
//                   multiboot in it; its grub.cfg on partition 1)
//   hdNp1, 0x83     ext2, "C4IX": /boot (c4pc.elf, c4pc64.elf, the
//                   kernel, grub/grub.cfg) and /home -- the person's,
//                   mounted at /home at every boot (init --home)
//   hdNp2, 0xDA     the system's files (the boot disc's disk.tar), as
//                   they are, sector for sector: GRUB hands them to c4pc
//                   as the boot drive by a block list
//
// The files come from c4pc's boot disc (the CD with volume id C4PC),
// mounted at /mnt/c4pc if it is not already. The boot loader's two
// images and the menu are its /boot/grub/c4pc-boot.img, c4pc-core.img
// and disk.cfg, which the ISO's build makes (Makefile, c4pc.iso).

#include "c4ix_user.h"

enum { C4I_MMU = 0x80000, DISK_COUNT = 0x188 };
enum { SYS_MOUNT = 248, SYS_UMOUNT = 249, SYS_MKFS = 250, SYS_SYNC = 252, SYS_BLKIO = 255 };
enum { RUN = 64, BUFSZ = 32768, CORE_MAX = 2047 * 512, PATHW = 256, START = 2048, SLACK = 131072 };

static char buf[BUFSZ];
static char core[CORE_MAX];
static char text[16384];
static char line[PATHW];
static char src[PATHW];

static int ksys(int num, int a, int b, int c) { return __c4_opcode(c, b, a, num); }
// COUNT sectors (to 64) at LBA of device DEV, from (WRITE) or into BUF
static int blkio(int dev, int lba, int count, char *b, int write) {
    return ksys(SYS_BLKIO, dev + (write ? 256 : 0) + (count - 1) * 65536, lba, (int)b);
}
static int num(char *s) {
    int v;
    v = 0;
    while (*s >= '0' && *s <= '9') { v = v * 10 + *s - '0'; ++s; }
    return v;
}
static void put32(char *p, int v) { p[0] = v & 255; p[1] = (v >> 8) & 255; p[2] = (v >> 16) & 255; p[3] = (v >> 24) & 255; }
static char *cat(char *to, char *a, char *b) { ustrcpy(to, a); ustrcpy(to + ustrlen(to), b); return to; }

// a disk's sectors and read-only flag, from /proc/disks ("hd0 SECTORS rw"):
// the sectors, or -1 if there is no such disk
static int disk_size(char *name, int *ro) {
    char *p, *w;
    int fd, got, r, n;
    if ((fd = uopen("/proc/disks", O_RD)) < 0) return 0 - 1;
    got = 0;
    while (got < 16383 && (r = uread(fd, text + got, 16383 - got)) > 0) got = got + r;
    uclose(fd);
    text[got] = 0;
    p = text;
    while (*p) {
        w = p; while (*p && *p != ' ') ++p; if (*p) { *p = 0; ++p; }
        n = num(p);
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
    int fd, got, r, k;
    if ((fd = uopen("/proc/mounts", O_RD)) < 0) return 0;
    got = 0;
    while (got < 16383 && (r = uread(fd, text + got, 16383 - got)) > 0) got = got + r;
    uclose(fd);
    text[got] = 0;
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
    int n, k, fd, got, r, id, size;
    if (is_file("/mnt/c4pc/boot/c4pc.elf", &size)) return 1;
    if (is_file("/mnt/cdrom/boot/c4pc.elf", &size)) { ustrcpy(src, "/mnt/cdrom"); return 1; }
    n = *(int *)DISK_COUNT;
    k = 2;
    while (k < n && k < 10) {
        name[0] = '0' + k; name[1] = ':'; ustrcpy(name + 2, ".c4pc-drive");
        if ((fd = uopen(name, O_RD)) >= 0) {
            got = 0;
            while (got < 4095 && (r = uread(fd, text + got, 4095 - got)) > 0) got = got + r;
            uclose(fd);
            text[got] = 0;
            p = text;
            while (*p) {
                if (line_is(p, "label C4PC")) {
                    name[0] = '0' + k; name[1] = 0;
                    av[0] = "mount"; av[1] = name; av[2] = "/mnt/c4pc"; av[3] = 0;
                    if ((id = spawn("/bin/mount", 3, av)) < 0 || uwait(id) != 0) return 0;
                    return is_file("/mnt/c4pc/boot/c4pc.elf", &size);
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
    cat(line, src, "/boot/");
    ustrcpy(line + ustrlen(line), name);
    if ((fd = uopen(line, O_RD)) < 0) { ufprintf(STDERR, "install: %s is not on the boot disc\n", line); return 0 - 1; }
    got = 0;
    while (got < max && (r = uread(fd, to + got, max - got)) > 0) got = got + r;
    uclose(fd);
    return got;
}
// the boot disc's /boot/NAME to the new disk's /boot/NAME
static int copy_file(char *name, char *to) {
    int in, out, r, total;
    cat(line, src, "/boot/");
    ustrcpy(line + ustrlen(line), name);
    if ((in = uopen(line, O_RD)) < 0) { ufprintf(STDERR, "install: %s is not on the boot disc\n", line); return 0 - 1; }
    if ((out = uopen(to, O_WR + O_CREATE + O_TRUNCATE)) < 0) { uclose(in); ufprintf(STDERR, "install: %s cannot be made\n", to); return 0 - 1; }
    total = 0;
    while ((r = uread(in, buf, BUFSZ)) > 0) {
        if (write(out, buf, r) != r) { uclose(in); uclose(out); ufprintf(STDERR, "install: %s: a write failed (the disk full?)\n", to); return 0 - 1; }
        total = total + r;
    }
    uclose(in);
    uclose(out);
    uprintf("install:   /boot/%s, %d KB\n", name, (total + 1023) / 1024);
    return 0;
}

int main(int argc, char **argv) {
    char *disk, *p;
    char dev1[8], dev2[8];
    int k, yes, d, sectors, ro, tar, tars, p2, p1, n, fd, r, lba, coresz, size, cfgn, i, j;
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
    if ((sectors = disk_size(disk, &ro)) < 0) { ufprintf(STDERR, "install: there is no disk %s (cat /proc/disks lists them)\n", disk); return 1; }
    if (ro) { ufprintf(STDERR, "install: %s is read-only\n", disk); return 1; }
    if (disk_busy(disk)) { ufprintf(STDERR, "install: %s, or a partition of it, is mounted: umount it first\n", disk); return 1; }
    ustrcpy(src, "/mnt/c4pc");
    if (!boot_disc()) { ufprintf(STDERR, "install: c4pc's boot disc (the CD it booted from) is not in a drive: boot from it to install\n"); return 1; }
    cat(line, src, "/boot/disk.tar");
    if (!is_file(line, &tar)) { ufprintf(STDERR, "install: %s is not there\n", line); return 1; }
    // the layout: the system's files in a partition at the end, their size
    // and 64 MB to spare; the rest from sector 2048 the person's, ext2
    tars = (tar + 511) / 512;
    p2 = tars + SLACK;
    p2 = (p2 + 2047) / 2048 * 2048;
    p1 = sectors - START - p2;
    p1 = p1 / 2048 * 2048;
    if (p1 < 65536) {
        ufprintf(STDERR, "install: %s is %d MB: too small (the system's files are %d MB, and 32 MB more are wanted for /boot and /home)\n",
            disk, sectors / 2048, (tar + 1048575) / 1048576);
        return 1;
    }
    uprintf("\n");
    uprintf("  install: C4IX onto %s, a disk of %d MB.\n", disk, sectors / 2048);
    uprintf("\n");
    uprintf("  WARNING: EVERYTHING ON %s WILL BE ERASED: its partitions, its files,\n", disk);
    uprintf("  whatever system was on it. This cannot be undone.\n");
    uprintf("\n");
    uprintf("  It becomes: GRUB to boot from, %s's partition 1 (%d MB, ext2: /boot\n", disk, p1 / 2048);
    uprintf("  and /home) and partition 2 (%d MB: the system's files).\n", p2 / 2048);
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
    // the MBR: boot.img's code (440 bytes), a disk signature, the table
    i = 440;
    while (i < 510) { buf[i] = 0; ++i; }
    put32(buf + 440, ucycles() | 1);
    // 1: active, ext2 (0x83), from 2048; 2: 0xDA (data, not a filesystem)
    p = buf + 446;
    p[0] = 0x80; p[1] = 0xFE; p[2] = 0xFF; p[3] = 0xFF; p[4] = 0x83; p[5] = 0xFE; p[6] = 0xFF; p[7] = 0xFF;
    put32(p + 8, START); put32(p + 12, p1);
    p = buf + 462;
    p[0] = 0; p[1] = 0xFE; p[2] = 0xFF; p[3] = 0xFF; p[4] = 0xDA; p[5] = 0xFE; p[6] = 0xFF; p[7] = 0xFF;
    put32(p + 8, START + p1); put32(p + 12, p2);
    buf[510] = 0x55; buf[511] = 0xAA;
    if (blkio(d, 0, 1, buf, 1) < 0) { ufprintf(STDERR, "install: %s's first sector could not be written\n", disk); return 1; }
    ustrcpy(dev1, disk); ustrcpy(dev1 + 3, "p1");
    ustrcpy(dev2, disk); ustrcpy(dev2 + 3, "p2");
    // partition 1: ext2, "C4IX"
    uprintf("install: an ext2 filesystem on %s\n", dev1);
    if (ksys(SYS_MKFS, 8 + d * 4, 0, (int)"C4IX") < 0) { ufprintf(STDERR, "install: %s could not be made ext2 (dmesg says why)\n", dev1); return 1; }
    umkdir("/mnt");
    umkdir("/mnt/install");
    if (ksys(SYS_MOUNT, 8 + d * 4, (int)"/mnt/install", 0) < 0) { ufprintf(STDERR, "install: %s could not be mounted at /mnt/install (dmesg says why)\n", dev1); return 1; }
    umkdir("/mnt/install/boot");
    umkdir("/mnt/install/boot/grub");
    umkdir("/mnt/install/home");
    uprintf("install: c4pc and the kernel into %s's /boot\n", dev1);
    if (copy_file("c4pc.elf", "/mnt/install/boot/c4pc.elf") < 0) return 1;
    if (copy_file("c4pc64.elf", "/mnt/install/boot/c4pc64.elf") < 0) return 1;
    if (copy_file("c4ix32.c4r", "/mnt/install/boot/c4ix32.c4r") < 0) return 1;
    // the menu: the boot disc's disk.cfg, its @DISK@ the block list of
    // partition 2 and @HOME@ partition 1's name
    if ((cfgn = read_all("grub/disk.cfg", text, 12000)) <= 0) return 1;
    text[cfgn] = 0;
    if ((fd = uopen("/mnt/install/boot/grub/grub.cfg", O_WR + O_CREATE + O_TRUNCATE)) < 0) { ufprintf(STDERR, "install: grub.cfg cannot be made\n"); return 1; }
    i = 0;
    while (text[i]) {
        if (text[i] == '@' && text[i + 1] == 'D' && text[i + 2] == 'I' && text[i + 3] == 'S' && text[i + 4] == 'K' && text[i + 5] == '@') {
            // (hd0,msdos2)0+N: GRUB's BIOS disk 0, the one it booted from
            ustrcpy(line, "(hd0,msdos2)0+");
            j = ustrlen(line);
            k = 1000000000;
            r = 0;
            while (k > 0) { if (tars / k % 10 || r || k == 1) { line[j] = '0' + tars / k % 10; ++j; r = 1; } k = k / 10; }
            line[j] = 0;
            write(fd, line, j);
            i = i + 6;
        } else if (text[i] == '@' && text[i + 1] == 'H' && text[i + 2] == 'O' && text[i + 3] == 'M' && text[i + 4] == 'E' && text[i + 5] == '@') {
            write(fd, dev1, ustrlen(dev1));
            i = i + 6;
        } else { write(fd, text + i, 1); ++i; }
    }
    uclose(fd);
    uprintf("install:   /boot/grub/grub.cfg\n");
    // partition 2: disk.tar, sector for sector
    uprintf("install: the system's files, %d MB, into %s\n", (tar + 1048575) / 1048576, dev2);
    cat(line, src, "/boot/disk.tar");
    if ((fd = uopen(line, O_RD)) < 0) { ufprintf(STDERR, "install: %s cannot be read\n", line); return 1; }
    lba = 0;
    size = 0;
    while ((r = uread(fd, buf, BUFSZ)) > 0) {
        // whole sectors: the last one's rest zeros
        while (r % 512) { buf[r] = 0; ++r; }
        if (blkio(8 + d * 4 + 1, lba, r / 512, buf, 1) < 0) { uclose(fd); ufprintf(STDERR, "install: %s could not be written at sector %d\n", dev2, lba); return 1; }
        lba = lba + r / 512;
        size = size + r;
        if (lba % 40960 == 0) uprintf("install:   %d MB\n", lba / 2048);
    }
    uclose(fd);
    if (lba != tars) { ufprintf(STDERR, "install: %d sectors of the system's files written, %d wanted\n", lba, tars); return 1; }
    ksys(SYS_SYNC, 0, 0, 0);
    if (ksys(SYS_UMOUNT, (int)"/mnt/install", 0, 0) < 0) ufprintf(STDERR, "install: /mnt/install could not be unmounted: it is written, but shut down before you boot from it\n");
    uprintf("\n");
    uprintf("  install: done. C4IX boots from %s now: take the CD out (or boot from the\n", disk);
    uprintf("  disk first, the VM's BIOS settings) and reboot. Your files go in /home,\n");
    uprintf("  which is %s's, and stay there.\n", dev1);
    uprintf("\n");
    return 0;
}

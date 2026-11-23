// C4IX userland: mount -- a host drive's files under a directory.
//
//   mount                 what is mounted, and the drives and disks there are
//   mount [DRIVE] [DIR]   DRIVE's files at DIR: a drive's number, or
//                         cdrom (the default), the first CD drive with a
//                         disc in it, c4pc's own boot disc last; DIR
//                         /mnt/cdrom by default
//   mount [-r] hdN [DIR]  disk N's ext2 filesystem at DIR (/mnt/hdN by
//                         default), read-only with -r: the kernel's own
//                         (ext2.c), every change written to the disk as it
//                         is made, nothing for persist to keep
//
// A mount is a host directory (vfs.c vfs_lazydir), as the games' are:
// nothing of it is made until something looks in it, and then the
// drive's listing of that directory (N:DIR/.c4ix-dir) says what is
// there; a file is read from the drive when it is read. So a disc of any
// size mounts at once, and costs memory only for what is copied off it:
// cp a file to /home and it is a RAM file like any other, which persist
// keeps (sync). The mount itself is not kept: persist does not go into
// a host directory, and the record of it is /tmp/mounts, which a boot
// starts without.
//
// What a drive is: N:.c4pc-drive, which c4pc makes for each of its
// drives ("kind cdrom", "label ...", "size KB", a line each; pc/cdrom.c,
// dev.c). A host that has none (libjs) has drives of unknown kind, which
// mount takes by number. Each mount of a CD drive looks at its disc again
// first (DISK_RESCAN), so a disc changed while unmounted is the new one.
// umount ejects it.

#include "c4ix_user.h"

enum { C4I_C4MJS = 8, C4I_MMU = 0x80000 };
enum { DISK_COUNT = 0x188, DISK_RESCAN = 0x194 };
enum { BUF = 2048, PATHW = 256, SYS_MOUNT = 248 };

static char info[BUF];
static char tab[BUF * 2];
static char proc[BUF];

static int ksys(int num, int a, int b, int c) { return __c4_opcode(c, b, a, num); }
// /proc/NAME into proc
static void proc_read(char *name) {
    int fd, got, r;
    proc[0] = 0;
    if ((fd = uopen(name, O_RD)) < 0) return;
    got = 0;
    while (got < BUF - 1 && (r = uread(fd, proc + got, BUF - 1 - got)) > 0) got = got + r;
    uclose(fd);
    proc[got] = 0;
}

// N:.c4pc-drive into info: its length, or -1 where the host has none
static int drive_info(int n) {
    char name[24];
    int fd, got, r;
    name[0] = '0' + n; name[1] = ':'; ustrcpy(name + 2, ".c4pc-drive");
    if (n > 9) return 0 - 1;
    if ((fd = uopen(name, O_RD)) < 0) { info[0] = 0; return 0 - 1; }
    got = 0;
    while (got < BUF - 1 && (r = uread(fd, info + got, BUF - 1 - got)) > 0) got = got + r;
    uclose(fd);
    info[got] = 0;
    return got;
}
// the value of info's line KEY into out (at most max), or "" where it has none
static char *field(char *key, char *out, int max) {
    char *p, *q;
    int kl, n;
    kl = ustrlen(key);
    p = info;
    out[0] = 0;
    while (*p) {
        q = p;
        n = 0;
        while (n < kl && q[n] == key[n]) ++n;
        if (n == kl && q[n] == ' ') {
            q = q + n + 1;
            n = 0;
            while (*q && *q != 10 && n < max - 1) { out[n] = *q; ++n; ++q; }
            out[n] = 0;
            return out;
        }
        while (*p && *p != 10) ++p;
        if (*p) ++p;
    }
    return out;
}
static int num(char *s) {
    int v;
    v = 0;
    while (*s >= '0' && *s <= '9') { v = v * 10 + *s - '0'; ++s; }
    return v;
}

// /tmp/mounts into tab: a line each, "DIR DRIVE KIND LABEL"
static int tab_read() {
    int fd, got, r;
    tab[0] = 0;
    if ((fd = uopen("/tmp/mounts", O_RD)) < 0) return 0;
    got = 0;
    while (got < BUF * 2 - 1 && (r = uread(fd, tab + got, BUF * 2 - 1 - got)) > 0) got = got + r;
    uclose(fd);
    tab[got] = 0;
    return got;
}
// is DIR (or drive N, if N >= 0) mounted: 1
static int mounted(char *dir, int n) {
    char *p;
    int dl, k;
    dl = ustrlen(dir);
    p = tab;
    while (*p) {
        k = 0;
        while (k < dl && p[k] == dir[k]) ++k;
        if (dl && k == dl && p[k] == ' ') return 1;
        k = 0;
        while (p[k] && p[k] != ' ' && p[k] != 10) ++k;
        if (n >= 0 && p[k] == ' ' && num(p + k + 1) == n && p[k + 1] >= '0' && p[k + 1] <= '9') return 1;
        while (*p && *p != 10) ++p;
        if (*p) ++p;
    }
    return 0;
}

static int list() {
    char kind[32], label[64], fs[48], media[8], d[PATHW];
    char *p, *dev, *at, *fsn, *how;
    int n, k, any, i;
    uprintf("/ on ram (C4IX's files, in memory)\n");
    tab_read();
    if (tab[0]) {
        p = tab;
        while (*p) {
            i = 0;
            while (*p && *p != ' ' && *p != 10 && i < PATHW - 1) { d[i] = *p; ++i; ++p; }
            d[i] = 0;
            if (*p == ' ') ++p;
            k = num(p);
            uprintf("%s on drive %d", d, k);
            if (drive_info(k) > 0) {
                field("kind", kind, 32); field("label", label, 64); field("fs", fs, 48);
                uprintf(" (%s", kind);
                if (label[0]) uprintf(" '%s'", label);
                if (fs[0]) uprintf(", %s", fs);
                uprintf(")");
            }
            uprintf("\n");
            while (*p && *p != 10) ++p;
            if (*p) ++p;
        }
    }
    // the disks' (/proc/mounts: "hd0 /mnt/disk ext2 rw")
    proc_read("/proc/mounts");
    p = proc;
    while (*p) {
        dev = p; while (*p && *p != ' ') ++p; if (*p) { *p = 0; ++p; }
        at = p; while (*p && *p != ' ') ++p; if (*p) { *p = 0; ++p; }
        fsn = p; while (*p && *p != ' ') ++p; if (*p) { *p = 0; ++p; }
        how = p; while (*p && *p != 10) ++p; if (*p) { *p = 0; ++p; }
        uprintf("%s on %s (%s, %s)\n", at, dev, fsn, how);
    }
    proc_read("/proc/disks");
    if (proc[0]) {
        uprintf("disks:\n");
        p = proc;
        while (*p) {
            dev = p; while (*p && *p != ' ') ++p; if (*p) { *p = 0; ++p; }
            uprintf("  %s: %d MB", dev, num(p) / 2048);
            while (*p && *p != ' ' && *p != 10) ++p;
            if (*p == ' ' && p[1] == 'r' && p[2] == 'o') uprintf(", read-only");
            while (*p && *p != 10) ++p; if (*p) ++p;
            uprintf("\n");
        }
    }
    if (!(__c4_info() & C4I_C4MJS)) return 0;
    n = *(int *)DISK_COUNT;
    any = 0;
    k = 0;
    while (k < n) {
        if (drive_info(k) > 0) {
            field("kind", kind, 32); field("label", label, 64); field("media", media, 8);
            if (!any) { uprintf("drives:\n"); any = 1; }
            uprintf("  %d: %s", k, kind);
            if (media[0] && media[0] == 'n') uprintf(", no disc");
            else if (label[0]) uprintf(", '%s'", label);
            if (k == 0) uprintf(", the boot disk");
            if (k == 1 && kind[0] == 's') uprintf(", where persist keeps changes");
            if (mounted("", k)) uprintf(", mounted");
            uprintf("\n");
        }
        ++k;
    }
    return 0;
}

// every directory on the way to DIR, made
static void mkdirs(char *dir) {
    char p[PATHW];
    int i;
    i = 0;
    while (dir[i] && i < PATHW - 1) {
        p[i] = dir[i];
        if (i > 0 && dir[i] == '/') { p[i] = 0; umkdir(p); p[i] = '/'; }
        ++i;
    }
    p[i] = 0;
    umkdir(p);
}

// the CD drive to mount: one with a disc, c4pc's own boot disc (C4PC)
// only if it is the only one; -1 for none
static int pick_cdrom(int n) {
    char kind[32], media[8], label[64];
    int k, own;
    own = 0 - 1;
    k = 0;
    while (k < n) {
        if (drive_info(k) > 0 && !ustrcmp(field("kind", kind, 32), "cdrom")) {
            *(int *)DISK_RESCAN = k;
            drive_info(k);
            if (field("media", media, 8)[0] == 'y' && !mounted("", k)) {
                field("label", label, 64);
                if (ustrcmp(label, "C4PC")) return k;
                if (own < 0) own = k;
            }
        }
        ++k;
    }
    return own;
}

int main(int argc, char **argv) {
    char *dir, *what;
    char host[8], kind[32], label[64], size[16], line[PATHW + 96];
    int n, k, fd, ro, st[4];
    // the drives' registers: under the MMU a task has them only when it asks
    if (__c4_info() & C4I_MMU) uioperm(UIOPERM_DEV);
    if (argc < 2) return list();
    // a disk: the kernel's ext2
    k = 1; ro = 0;
    if (!ustrcmp(argv[k], "-r")) { ro = 1; ++k; }
    if (k < argc && argv[k][0] == 'h' && argv[k][1] == 'd' && argv[k][2] >= '0' && argv[k][2] <= '9') {
        n = num(argv[k] + 2);
        if (k + 1 < argc) dir = argv[k + 1];
        else { ustrcpy(line, "/mnt/"); ustrcpy(line + 5, argv[k]); dir = line; }
        if (dir[0] != '/') { ufprintf(STDERR, "mount: %s: the directory must be a whole path, from /\n", dir); return 1; }
        mkdirs(dir);
        if (ksys(SYS_MOUNT, n, (int)dir, ro) < 0) {
            ufprintf(STDERR, "mount: hd%d on %s: not an ext2 filesystem, or %s is not an empty directory (dmesg says which; mkfs hd%d makes one)\n", n, dir, dir, n);
            return 1;
        }
        uprintf("mount: hd%d on %s%s\n", n, dir, ro ? ", read-only" : "");
        return 0;
    }
    if (!(__c4_info() & C4I_C4MJS)) { ufprintf(STDERR, "mount: this host has no drives to mount\n"); return 1; }
    what = argv[1];
    dir = "/mnt/cdrom";
    if (argc > 2) dir = argv[2];
    else if (what[0] == '/') { dir = what; what = "cdrom"; }
    if (dir[0] != '/') { ufprintf(STDERR, "mount: %s: the directory must be a whole path, from /\n", dir); return 1; }
    n = *(int *)DISK_COUNT;
    tab_read();
    if (mounted(dir, 0 - 1)) { ufprintf(STDERR, "mount: something is mounted at %s already: umount it first\n", dir); return 1; }
    if (!ustrcmp(what, "cdrom")) {
        k = pick_cdrom(n);
        if (k < 0) { ufprintf(STDERR, "mount: no CD drive with a disc in it\n"); return 1; }
    } else if (what[0] >= '0' && what[0] <= '9' && !what[1]) {
        k = what[0] - '0';
        if (k >= n) { ufprintf(STDERR, "mount: there is no drive %d (%d drives)\n", k, n); return 1; }
        if (k < 2) { ufprintf(STDERR, "mount: drive %d is the system's own (the boot disk, or persist's)\n", k); return 1; }
        if (mounted("", k)) { ufprintf(STDERR, "mount: drive %d is mounted already\n", k); return 1; }
        *(int *)DISK_RESCAN = k;
    } else { ufprintf(STDERR, "usage: mount [cdrom | DRIVE] [DIR], or mount [-r] hdN [DIR]\n"); return 1; }
    kind[0] = 0; label[0] = 0; size[0] = 0;
    if (drive_info(k) > 0) {
        field("kind", kind, 32); field("label", label, 64); field("size", size, 16);
        if (!ustrcmp(kind, "cdrom") && field("media", line, 8)[0] != 'y') {
            ufprintf(STDERR, "mount: drive %d has no disc in it\n", k); return 1;
        }
    }
    mkdirs(dir);
    if (ustat(dir, st) < 0 || st[0] != 1) { ufprintf(STDERR, "mount: %s is not a directory\n", dir); return 1; }
    if (ureaddir(dir, 0, line) >= 0) { ufprintf(STDERR, "mount: %s has things in it already\n", dir); return 1; }
    host[0] = '0' + k; host[1] = ':'; host[2] = '/'; host[3] = 0;
    if (ulazyfile(dir, host, 0 - 1) < 0) { ufprintf(STDERR, "mount: %s cannot be made drive %d's\n", dir, k); return 1; }
    // the record: the table again, with this line on the end
    tab_read();
    if ((fd = uopen("/tmp/mounts", O_WR + O_CREATE + O_TRUNCATE)) >= 0) {
        if (tab[0]) write(fd, tab, ustrlen(tab));
        ustrcpy(line, dir);
        n = ustrlen(line);
        line[n] = ' '; line[n + 1] = '0' + k; line[n + 2] = ' '; line[n + 3] = 0;
        ustrcpy(line + n + 3, kind[0] ? kind : "drive");
        n = ustrlen(line);
        line[n] = 10; line[n + 1] = 0;
        write(fd, line, ustrlen(line));
        uclose(fd);
    }
    uprintf("mount: drive %d", k);
    if (kind[0]) {
        uprintf(" (%s", kind);
        if (label[0]) uprintf(" '%s'", label);
        if (size[0]) uprintf(", %d MB", num(size) / 1024);
        uprintf(")");
    }
    uprintf(" on %s\n", dir);
    return 0;
}

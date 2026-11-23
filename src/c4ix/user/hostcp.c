// C4IX userland: hostcp -- files onto the host's shared folders (VMware's,
// by c4pc's VMware drive: src/c4pc/pc/vmware.c, docs/c4pc.md "VMware").
//
//   hostcp                     the host's shared folders, and the drive
//   hostcp FILE... SHARE[/DIR] each FILE into that folder on the host
//
// Reading them is a mount: `mount N /mnt/host` (N, the drive `mount` lists
// as vmware), then /mnt/host/hgfs/SHARE/... -- the host's files, read when
// they are read. A mount keeps what is written there in RAM, as a CD's
// does, so writing to the host is this: each file is written whole to
// the drive's hgfs/SHARE/DIR/NAME, which c4pc hands to VMware (HGFS) when
// it is closed. A folder already listed by a mount shows a file written
// since only after umount and mount again.

#include "c4ix_user.h"

enum { BUF = 32768, PATHW = 256, D_FD = 0x124, D_WNAME = 0x178, D_WADDR = 0x17c, D_WLEN = 0x180,
       D_WCLOSE = 0x184, C4I_MMU = 0x80000 };

static char buf[BUF];
static char text[8192];

static int has_line(char *p, char *want) {
    int k;
    while (*p) {
        k = 0;
        while (want[k] && p[k] == want[k]) ++k;
        if (!want[k] && (p[k] == 10 || p[k] == 13 || !p[k])) return 1;
        while (*p && *p != 10) ++p;
        if (*p) ++p;
    }
    return 0;
}
// the drive whose N:.c4pc-drive says "kind vmware", or -1
static int vmw_drive() {
    char name[16];
    int k;
    k = 2;
    while (k < 10) {
        name[0] = '0' + k; name[1] = ':'; ustrcpy(name + 2, ".c4pc-drive");
        if (uslurp(name, text, 8192) > 0 && has_line(text, "kind vmware")) return k;
        ++k;
    }
    return 0 - 1;
}
static char *base(char *p) {
    char *b;
    b = p;
    while (*p) { if (*p == '/' && p[1]) b = p + 1; ++p; }
    return b;
}
static int put(int drv, char *src, char *dir) {
    char name[PATHW];
    int in, fd, n, bad, total;
    name[0] = '0' + drv; name[1] = ':'; name[2] = 0;
    ustrcat(name, "hgfs/");
    if (ustrlen(name) + ustrlen(dir) + ustrlen(base(src)) + 2 >= PATHW) { ufprintf(STDERR, "hostcp: %s: the name is too long\n", src); return 1; }
    ustrcat(name, dir);
    if (dir[0] && dir[ustrlen(dir) - 1] != '/') ustrcat(name, "/");
    ustrcat(name, base(src));
    if ((in = uopen(src, O_RD)) < 0) { ufprintf(STDERR, "hostcp: cannot read %s\n", src); return 1; }
    *(int *)D_WNAME = (int)name;
    fd = *(int *)D_WNAME;
    if (fd < 0) { uclose(in); ufprintf(STDERR, "hostcp: the drive will not take %s\n", name); return 1; }
    bad = 0; total = 0;
    while ((n = uread(in, buf, BUF)) > 0) {
        *(int *)D_FD = fd; *(int *)D_WADDR = (int)buf; *(int *)D_WLEN = n;
        if (*(int *)D_WLEN != n) { bad = 1; break; }
        total = total + n;
    }
    if (n < 0) bad = 1;
    uclose(in);
    *(int *)D_WCLOSE = fd;
    if (*(int *)D_WCLOSE < 0) bad = 1;
    if (bad) { ufprintf(STDERR, "hostcp: %s: not all of it was written\n", src); return 1; }
    uprintf("hostcp: %s -> %s (%d bytes)\n", src, name + 2, total);
    return 0;
}

int main(int argc, char **argv) {
    char name[32], *p;
    int drv, a, bad, n;
    if ((drv = vmw_drive()) < 0) { ufprintf(STDERR, "hostcp: no VMware drive (this is not VMware, or c4pc ran with --no-vmware)\n"); return 1; }
    if (__c4_info() & C4I_MMU) uioperm(UIOPERM_DEV);
    if (argc < 2) {
        name[0] = '0' + drv; name[1] = ':'; ustrcpy(name + 2, "hgfs/.c4ix-dir");
        uprintf("VMware's drive is %d: mount %d /mnt/host reads the shared folders, at /mnt/host/hgfs\n", drv, drv);
        if ((n = uslurp(name, text, 8192)) < 0) { uprintf("no shared folders (VM settings, Options, Shared Folders)\n"); return 0; }
        p = text;
        while (*p) {
            if (p[0] == 'd' && p[1] == ' ') { uprintf("  "); p = p + 2; while (*p && *p != 10) { uprintf("%c", *p); ++p; } uprintf("\n"); }
            while (*p && *p != 10) ++p;
            if (*p) ++p;
        }
        return 0;
    }
    if (argc < 3) { ufprintf(STDERR, "usage: hostcp FILE... SHARE[/DIR]   (hostcp alone lists the shares)\n"); return 1; }
    bad = 0;
    a = 1;
    while (a < argc - 1) { bad = bad + put(drv, argv[a], argv[argc - 1]); ++a; }
    return bad ? 1 : 0;
}

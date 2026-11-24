// C4IX userland: eject -- a CD drive's disc out.
//
//   eject [cdrom | DRIVE]   the first CD drive, or drive DRIVE
//
// Mounted, the disc is let go first (umount, which then ejects it);
// otherwise the drive is told at once (DISK_EJECT: c4pc's cdrom.c sends
// ALLOW MEDIUM REMOVAL, then START STOP UNIT with LoEj, and VMware and
// QEMU take the ISO out of the drive). The user asked for it: booting an
// installed system again with no trip through VMware's menus.

#include "c4ix_user.h"

enum { C4I_MMU = 0x80000, DISK_COUNT = 0x188, DISK_EJECT = 0x190, BUF = 2048 };

static char text[BUF];

// is drive K a CD drive (its N:.c4pc-drive says "kind cdrom")
static int is_cd(int k) {
    char name[16];
    char *p;
    int n;
    name[0] = '0' + k; name[1] = ':'; ustrcpy(name + 2, ".c4pc-drive");
    if ((n = uslurp(name, text, BUF - 1)) < 0) return 0;
    text[n] = 0;
    p = text;
    while (*p) {
        if (!ustrncmp(p, "kind cdrom", 10)) return 1;
        while (*p && *p != 10) ++p;
        if (*p) ++p;
    }
    return 0;
}
// is drive K mounted (/tmp/mounts: "DIR K kind" a line)
static int mounted(int k) {
    char *p;
    int n;
    if ((n = uslurp("/tmp/mounts", text, BUF - 1)) < 0) return 0;
    text[n] = 0;
    p = text;
    while (*p) {
        while (*p && *p != ' ' && *p != 10) ++p;
        if (*p == ' ') { ++p; if (uatoi(p) == k && *p >= '0' && *p <= '9') return 1; }
        while (*p && *p != 10) ++p;
        if (*p) ++p;
    }
    return 0;
}

int main(int argc, char **argv) {
    char d[4], *av[3];
    int k, n, id;
    if (__c4_info() & C4I_MMU) uioperm(UIOPERM_DEV);
    n = *(int *)DISK_COUNT;
    k = 0 - 1;
    if (argc > 1 && argv[1][0] >= '0' && argv[1][0] <= '9') k = uatoi(argv[1]);
    else if (argc > 1 && ustrcmp(argv[1], "cdrom")) { ufprintf(STDERR, "usage: eject [cdrom | DRIVE]\n"); return 2; }
    else { k = 0; while (k < n && !is_cd(k)) ++k; if (k >= n) { ufprintf(STDERR, "eject: no CD drive here\n"); return 1; } }
    if (k < 0 || k >= n || !is_cd(k)) { ufprintf(STDERR, "eject: drive %d is not a CD drive\n", k); return 1; }
    if (mounted(k)) {
        // umount lets it go, and ejects it
        d[0] = '0' + k; d[1] = 0;
        av[0] = "umount"; av[1] = d; av[2] = 0;
        if ((id = spawn("/bin/umount", 2, av)) < 0) { ufprintf(STDERR, "eject: umount did not run\n"); return 1; }
        return uwait(id) ? 1 : 0;
    }
    *(int *)DISK_EJECT = k;
    uprintf("eject: drive %d's disc ejected\n", k);
    return 0;
}

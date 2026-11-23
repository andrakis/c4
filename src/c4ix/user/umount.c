// C4IX userland: umount -- a mount (mount.c) let go.
//
//   umount [DIR | cdrom | DRIVE]   /mnt/cdrom, or the one mount there is,
//                                  by default
//   umount -k ...                  ... and the disc left in its drive
//
// The directory's entries go (as rm -r takes them: only the made entries,
// nothing on the drive), and DIR stays, empty, for the next mount. A file
// still open there keeps the mount: umount says it is busy and leaves it.
// Then a CD drive's tray opens (DISK_EJECT; pc/cdrom.c), so that the
// machine's own drive can be given another disc.

#include "c4ix_user.h"

enum { C4I_C4MJS = 8, C4I_MMU = 0x80000 };
enum { DISK_EJECT = 0x190 };
enum { BUF = 4096, NAME_MAX = 64, PATHW = 256 };

static char tab[BUF];

static int num(char *s) {
    int v;
    v = 0;
    while (*s >= '0' && *s <= '9') { v = v * 10 + *s - '0'; ++s; }
    return v;
}

// DIR's contents, each directory as it empties; how many could not go
static int rmtree(char *dir) {
    char name[NAME_MAX], sub[PATHW];
    int i, r, bad, n;
    bad = 0;
    i = 0;
    while ((r = ureaddir(dir, i, name)) >= 0) {
        n = ustrlen(dir);
        if (n + ustrlen(name) + 2 > PATHW) { ++bad; ++i; continue; }
        ustrcpy(sub, dir);
        sub[n] = '/'; sub[n + 1] = 0;
        ustrcpy(sub + n + 1, name);
        if (r == 1) { n = rmtree(sub); if (!n && uunlink(sub) < 0) n = 1; }
        else n = uunlink(sub) < 0;
        if (n) { bad = bad + n; ++i; }
    }
    return bad;
}

int main(int argc, char **argv) {
    char d[PATHW], kind[32];
    char *want, *p, *line, *keep_from;
    int fd, got, r, a, keep, k, found, i, dl, bad, nlines;
    keep = 0;
    a = 1;
    if (argc > a && !ustrcmp(argv[a], "-k")) { keep = 1; ++a; }
    want = "";
    if (argc > a) want = argv[a];
    tab[0] = 0; got = 0;
    if ((fd = uopen("/tmp/mounts", O_RD)) >= 0) {
        while (got < BUF - 1 && (r = uread(fd, tab + got, BUF - 1 - got)) > 0) got = got + r;
        uclose(fd);
    }
    tab[got] = 0;
    // the line: by its directory, its drive's number, "cdrom" (its kind),
    // or with nothing asked /mnt/cdrom, or the only one
    nlines = 0; p = tab;
    while (*p) { ++nlines; while (*p && *p != 10) ++p; if (*p) ++p; }
    found = 0; line = tab; k = 0; kind[0] = 0;
    p = tab;
    while (*p && !found) {
        line = p;
        i = 0;
        while (*p && *p != ' ' && *p != 10 && i < PATHW - 1) { d[i] = *p; ++i; ++p; }
        d[i] = 0;
        if (*p == ' ') ++p;
        k = num(p);
        while (*p >= '0' && *p <= '9') ++p;
        if (*p == ' ') ++p;
        i = 0;
        while (*p && *p != 10 && i < 31) { kind[i] = *p; ++i; ++p; }
        kind[i] = 0;
        if (*p) ++p;
        if (!want[0] && (nlines == 1 || !ustrcmp(d, "/mnt/cdrom"))) found = 1;
        else if (want[0] && (!ustrcmp(want, d) || !ustrcmp(want, kind) ||
                 (want[0] >= '0' && want[0] <= '9' && !want[1] && want[0] - '0' == k))) found = 1;
    }
    if (!found) {
        if (want[0]) ufprintf(STDERR, "umount: %s is not mounted\n", want);
        else if (nlines) ufprintf(STDERR, "umount: which? %d things are mounted (mount lists them)\n", nlines);
        else ufprintf(STDERR, "umount: nothing is mounted\n");
        return 1;
    }
    bad = rmtree(d);
    if (bad) {
        ufprintf(STDERR, "umount: %s is busy: %d of its files are open, or could not go\n", d, bad);
        return 1;
    }
    // DIR emptied, and made again plain: a directory never looked in is
    // still the drive's until it is listed (vfs.c), and rmtree listed it
    uunlink(d);
    umkdir(d);
    // the table less the line
    dl = 0; while (line[dl] && line[dl] != 10) ++dl;
    if (line[dl]) ++dl;
    keep_from = line + dl;
    if ((fd = uopen("/tmp/mounts", O_WR + O_CREATE + O_TRUNCATE)) >= 0) {
        if (line > tab) write(fd, tab, line - tab);
        if (*keep_from) write(fd, keep_from, ustrlen(keep_from));
        uclose(fd);
    }
    if (!keep && !ustrcmp(kind, "cdrom") && (__c4_info() & C4I_C4MJS)) {
        if (__c4_info() & C4I_MMU) uioperm(UIOPERM_DEV);
        *(int *)DISK_EJECT = k;
        uprintf("umount: %s let go; drive %d's disc ejected\n", d, k);
    } else uprintf("umount: %s let go\n", d);
    return 0;
}

// C4IX userland: df -- how full each place files are is.
//
//   df        in KB
//   df -h     in MB (and GB), one decimal
//
// ram is C4IX's own files and everything else in memory: the heap
// (/proc/meminfo, which free prints in more detail). Then each of the
// host's drives that says what it is (N:.c4pc-drive, which c4pc makes:
// dev.c, pc/store.c, pc/cdrom.c): the boot disk, the store where persist
// keeps the changes (drive 1, on an IDE disk), a CD. What is saved to the
// store is counted there; a file written and not yet saved is ram's.
// And each disk mounted (mount hdN: ext2, the kernel's own), from its
// superblock's counts.

#include "c4ix_user.h"

enum { C4I_C4MJS = 8, C4I_MMU = 0x80000 };
enum { DISK_COUNT = 0x188 };
enum { BUF = 2048, PATHW = 256, SYS_STATFS = 251 };

static int ksys(int num, int a, int b, int c) { return __c4_opcode(c, b, a, num); }

static char info[BUF];
static char tab[BUF * 2];
static int human;

// the value of BUF's line KEY into out, "" where it has none
static char *field(char *buf, char *key, char *out, int max) {
    char *p, *q;
    int kl, n;
    kl = ustrlen(key);
    p = buf;
    out[0] = 0;
    while (*p) {
        n = 0;
        while (n < kl && p[n] == key[n]) ++n;
        if (n == kl && p[n] == ' ') {
            q = p + n + 1;
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

// K kilobytes in ten columns; -1 is "-"
static void amt(int k) {
    char s[16];
    int n, v;
    n = 15; s[n] = 0;
    if (k < 0) s[--n] = '-';
    else if (human) {
        if (k >= 1024 * 1024) { v = k / 1024 * 10 / 1024; s[--n] = 'G'; }
        else { v = k * 10 / 1024; s[--n] = 'M'; }
        s[--n] = '0' + v % 10; s[--n] = '.'; v = v / 10;
        s[--n] = '0' + v % 10; v = v / 10;
        while (v) { s[--n] = '0' + v % 10; v = v / 10; }
    } else {
        v = k;
        s[--n] = '0' + v % 10; v = v / 10;
        while (v) { s[--n] = '0' + v % 10; v = v / 10; }
    }
    while (n > 5) s[--n] = ' ';
    uprintf("%s", s + n);
}
static void row(char *name, int size, int used, int avail, char *on) {
    char s[24];
    int i, n;
    i = 0;
    while (name[i] && i < 18) { s[i] = name[i]; ++i; }
    while (i < 18) { s[i] = ' '; ++i; }
    s[i] = 0;
    uprintf("%s", s);
    amt(size); amt(used); amt(avail);
    if (size > 0 && used >= 0) {
        i = used / 1024 * 100 / (size / 1024 + 1);
        if (size < 1024 * 1024) i = used * 100 / size;
        if (i > 100) i = 100;
        s[0] = ' '; s[1] = ' '; s[2] = ' '; s[3] = ' '; s[4] = ' '; s[5] = '%'; s[6] = 0;
        n = 4;
        s[n] = '0' + i % 10; i = i / 10;
        while (i) { --n; s[n] = '0' + i % 10; i = i / 10; }
        uprintf("%s", s);
    } else uprintf("     -");
    uprintf(" %s\n", on);
}
// where drive N is mounted (/tmp/mounts), into out; "" if nowhere
static char *mounted_at(int n, char *out) {
    char *p;
    int i;
    out[0] = 0;
    p = tab;
    while (*p) {
        i = 0;
        while (*p && *p != ' ' && *p != 10 && i < PATHW - 1) { out[i] = *p; ++i; ++p; }
        out[i] = 0;
        if (*p == ' ') ++p;
        if (uatoi(p) == n && *p >= '0' && *p <= '9') return out;
        out[0] = 0;
        while (*p && *p != 10) ++p;
        if (*p) ++p;
    }
    return out;
}

int main(int argc, char **argv) {
    char mem[1024], v[64], kind[32], label[64], name[48], on[PATHW], d[24];
    char *p, *dev, *at;
    int heap, used, n, k, size, fs[8];
    human = argc > 1 && !ustrcmp(argv[1], "-h");
    // the drives' registers: under the MMU a task has them only when it asks
    if (__c4_info() & C4I_MMU) uioperm(UIOPERM_DEV);
    uprintf("Filesystem              Size      Used     Avail  Use%% Mounted on\n");
    uslurp("/proc/meminfo", mem, 1024);
    heap = uatoi(field(mem, "heap", v, 64));
    field(mem, "used", v, 64);
    used = 0 - 1;
    if (v[0]) used = uatoi(v);
    if (used >= 0) row("ram", heap, used, heap - used, "/");
    else row("ram", heap, 0 - 1, 0 - 1, "/");
    // the disks mounted: "hd0 /mnt/disk ext2 rw" a line of /proc/mounts
    uslurp("/proc/mounts", tab, BUF * 2);
    p = tab;
    while (*p) {
        dev = p; while (*p && *p != ' ') ++p; if (*p) { *p = 0; ++p; }
        at = p; while (*p && *p != ' ') ++p; if (*p) { *p = 0; ++p; }
        while (*p && *p != 10) ++p; if (*p) ++p;
        if (ksys(SYS_STATFS, (int)at, (int)fs, 0) < 0) continue;
        k = fs[0] / 1024;
        ustrcpy(name, dev); ustrcpy(name + ustrlen(name), " ext2");
        if (k > 0) row(name, fs[1] * k, (fs[1] - fs[2]) * k, fs[2] * k, at);
        else row(name, fs[1] / (1024 / fs[0]), (fs[1] - fs[2]) / (1024 / fs[0]), fs[2] / (1024 / fs[0]), at);
    }
    if (!(__c4_info() & C4I_C4MJS)) return 0;
    uslurp("/tmp/mounts", tab, BUF * 2);
    n = *(int *)DISK_COUNT;
    k = 0;
    while (k < n && k < 10) {
        d[0] = '0' + k; d[1] = ':'; ustrcpy(d + 2, ".c4pc-drive");
        if (uslurp(d, info, BUF) > 0) {
            field(info, "kind", kind, 32);
            field(info, "label", label, 64);
            if (ustrcmp(kind, "none")) {
                name[0] = '0' + k; name[1] = ':'; ustrcpy(name + 2, kind);
                if (label[0] && ustrlen(label) < 24) { ustrcpy(name + ustrlen(name), " "); ustrcpy(name + ustrlen(name), label); }
                mounted_at(k, on);
                if (!on[0]) {
                    if (k == 0) ustrcpy(on, "(the boot disk)");
                    else if (!ustrcmp(kind, "store")) ustrcpy(on, "(persist's: sync saves there)");
                    else if (!ustrcmp(kind, "cdrom") && field(info, "media", v, 8)[0] != 'y') ustrcpy(on, "(no disc)");
                    else ustrcpy(on, "-");
                }
                size = 0 - 1;
                if (field(info, "size", v, 64)[0]) size = uatoi(v);
                if (size >= 0) row(name, size, uatoi(field(info, "used", v, 64)), uatoi(field(info, "free", v, 64)), on);
                else row(name, 0 - 1, 0 - 1, 0 - 1, on);
            }
        }
        ++k;
    }
    return 0;
}

// C4IX userland: cp -- copy files.
//
//   cp SRC DST          a file to DST, or into DST if it is a directory
//   cp SRC... DIR       each into DIR
//   cp -r SRC... DST    a directory and everything in it too
//
// A copy is a RAM file like any other (persist keeps it at the next
// save); copied off a mounted disc (mount.c) it is read from the drive
// as it is copied, a buffer at a time.

#include "c4ix_user.h"

enum { BUF = 32768, NAME_MAX = 64, PATHW = 256 };

static char buf[BUF];
static int rec;

static int isdir(char *p) {
    int st[4];
    return ustat(p, st) == 0 && st[0] == 1;
}
// the last component of P
static char *base(char *p) {
    char *b;
    b = p;
    while (*p) { if (*p == '/' && p[1]) b = p + 1; ++p; }
    return b;
}
// A/B into out: 0, or -1 when it does not fit
static int join(char *out, char *a, char *b) { return upathjoin(out, PATHW, a, b); }

static int copy_file(char *src, char *dst) {
    int in, out, n, w, bad;
    if ((in = uopen(src, O_RD)) < 0) { ufprintf(STDERR, "cp: cannot read %s\n", src); return 1; }
    if ((out = uopen(dst, O_WR + O_CREATE + O_TRUNCATE)) < 0) {
        uclose(in);
        ufprintf(STDERR, "cp: cannot write %s\n", dst);
        return 1;
    }
    bad = 0;
    while ((n = uread(in, buf, BUF)) > 0) {
        w = write(out, buf, n);
        if (w != n) { ufprintf(STDERR, "cp: %s: out of room (memory)\n", dst); bad = 1; break; }
    }
    if (n < 0) { ufprintf(STDERR, "cp: %s: a read failed\n", src); bad = 1; }
    uclose(in);
    uclose(out);
    return bad;
}
static int copy(char *src, char *dst);
static int copy_dir(char *src, char *dst) {
    char name[NAME_MAX], s[PATHW], d[PATHW];
    int i, bad;
    if (!rec) { ufprintf(STDERR, "cp: %s is a directory: cp -r\n", src); return 1; }
    if (!isdir(dst) && umkdir(dst) < 0) { ufprintf(STDERR, "cp: cannot make %s\n", dst); return 1; }
    bad = 0;
    i = 0;
    while (ureaddir(src, i, name) >= 0) {
        if (join(s, src, name) < 0 || join(d, dst, name) < 0) { ++bad; ++i; continue; }
        bad = bad + copy(s, d);
        ++i;
    }
    return bad;
}
static int copy(char *src, char *dst) {
    if (isdir(src)) return copy_dir(src, dst);
    return copy_file(src, dst);
}

int main(int argc, char **argv) {
    char d[PATHW];
    char *dst;
    int a, k, bad, todir;
    a = 1;
    rec = 0;
    while (a < argc && argv[a][0] == '-' && argv[a][1]) {
        if (!ustrcmp(argv[a], "-r") || !ustrcmp(argv[a], "-R")) rec = 1;
        else { ufprintf(STDERR, "usage: cp [-r] SRC... DST\n"); return 1; }
        ++a;
    }
    if (argc - a < 2) { ufprintf(STDERR, "usage: cp [-r] SRC... DST\n"); return 1; }
    dst = argv[argc - 1];
    todir = isdir(dst);
    if (argc - a > 2 && !todir) { ufprintf(STDERR, "cp: %s is not a directory\n", dst); return 1; }
    bad = 0;
    k = a;
    while (k < argc - 1) {
        if (todir) {
            if (join(d, dst, base(argv[k])) < 0) { ++bad; ++k; continue; }
            bad = bad + copy(argv[k], d);
        } else bad = bad + copy(argv[k], dst);
        ++k;
    }
    return bad != 0;
}

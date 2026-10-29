// C4IX userland: persist -- the filesystem's changes, kept on a drive
// that outlives the machine (docs/c4ix-design.md, "Persistence").
//
//   persist boot ARCHIVE...  unpack the system (vfsload runs this as the
//                            boot's unpacking task), note what it is,
//                            then lay the saved changes over it; the
//                            unpacked system is kept on the drive, and
//                            the next boot unpacks that instead
//   persist save             write every change since boot to the drive
//   sync                     the same (/bin/sync is this program)
//   persist status           what save would write
//   persist forget           save nothing: the next boot starts clean
//   persist -d SECS          save every SECS seconds while things change
//
// C4IX's files are RAM. What lasts is host drive 1 -- in the browser an
// IndexedDB store the page keeps (libjs/page.js), on the command line
// the directory `cli.js -w` names. The changes go there as one tar,
// 1:c4ix-persist.tar, written through c4bb's disk write registers and
// replaced whole when it closes, so a save cut short leaves the last.
//
// What a change is. `boot` walks the tree once the root archive is in,
// and writes the list of every path there to /var/persist/base. That
// file's modification stamp (the VFS's order of changes, ustat out[3])
// is the epoch: a path not in the list, or stamped after the epoch, is
// a change, and a path in the list that is gone is a deletion. The saved
// changes are laid over the tree AFTER the list is made, so they count
// as changes again and the next save keeps them; deletions go in the
// tar as var/persist/whiteouts and are deleted again at the next boot.
// Files go in the order they were changed, so b4ke still sees an object
// newer than its source when they come back.
//
// /tmp, /log and /var/persist are never kept.
//
// The unpacked system, kept. Inflating the root archive is seconds of
// interpreted work at every boot, and the archive is megabytes to fetch.
// So where there is a drive, the tar it inflates to is kept there too,
// 1:c4ix-base.tar, and 1:c4ix-base.id names the archive it came from:
// the contents of NAME.id, a digest build-images.sh writes beside the
// archive, or else its size. A boot whose archive has that id untars the
// kept copy, and neither fetches nor inflates the archive. A new archive
// has another id and is unpacked again; Reset Files on the page empties
// the drive, and so does the same. Only for one archive, the root's.

#include "c4ix_user.h"
#include "archive.h"

enum {
    C4I_C4MJS = 8, C4I_MMU = 0x80000,
    DISK_FD = 0x124, DISK_WNAME = 0x178, DISK_WADDR = 0x17c, DISK_WLEN = 0x180,
    DISK_WCLOSE = 0x184, DISK_COUNT = 0x188,
    PATHMAX = 256, NAMEMAX = 64, CHUNK = 65536, BLOCK = 512
};

static char *SNAP = "1:c4ix-persist.tar";
static char *BASE = "/var/persist/base";
static char *WHITE = "/var/persist/whiteouts";
static char *KTAR = "1:c4ix-base.tar";
static char *KID = "1:c4ix-base.id";

// ---- the drive ----

// Host drive 1, and something there to write to. ANNOUNCED, NEVER
// PROBED: only libjs says it has c4bb's disk registers (C4I_C4MJS); on
// native c4m those addresses are ordinary memory.
static int have_drive() {
    if (!(__c4_info() & C4I_C4MJS)) return 0;
    return *(int *)DISK_COUNT >= 2;
}

static int wfd;
static int wbad;
static int wtotal;

static int w_open(char *name) {
    *(int *)DISK_WNAME = (int)name;
    wfd = *(int *)DISK_WNAME;
    wbad = wfd < 0;
    wtotal = 0;
    return wfd;
}

static void w_put(char *buf, int len) {
    if (wbad || len <= 0) return;
    *(int *)DISK_FD = wfd;
    *(int *)DISK_WADDR = (int)buf;
    *(int *)DISK_WLEN = len;
    if (*(int *)DISK_WLEN != len) wbad = 1;
    wtotal = wtotal + len;
}

static int w_close() {
    *(int *)DISK_WCLOSE = wfd;
    if (*(int *)DISK_WCLOSE < 0) wbad = 1;
    return wbad ? 0 - 1 : 0;
}

// ---- strings and paths ----

static int starts(char *s, char *p) {
    while (*p) { if (*s != *p) return 0; ++s; ++p; }
    return 1;
}

static int same(char *a, char *b) { return !ustrcmp(a, b); }

// never kept: what the system makes again itself, and this
static int excluded(char *path) {
    return same(path, "/tmp") || starts(path, "/tmp/") ||
           same(path, "/log") || starts(path, "/log/") ||
           same(path, "/proc") || starts(path, "/proc/") ||
           same(path, "/var/persist") || starts(path, "/var/persist/");
}

static char *dup(char *s) {
    char *r;
    int n;
    n = ustrlen(s);
    r = (char *)malloc(n + 1);
    memcpy(r, s, n + 1);
    return r;
}

// ---- the base: a set of paths ----

static char **hset;
static int hcap;
static int hcount;

static int hash(char *s) {
    int h;
    h = 5381;
    while (*s) { h = h * 33 + (*s & 255); ++s; }
    return h & 0x7fffffff;
}

static void hinit(int n) {
    hcap = 1024;
    while (hcap < n * 2) hcap = hcap * 2;
    hset = (char **)malloc(hcap * sizeof(int));
    memset((char *)hset, 0, hcap * sizeof(int));
    hcount = 0;
}

static void hadd(char *s) {
    int i;
    i = hash(s) & (hcap - 1);
    while (hset[i]) { if (same(hset[i], s)) return; i = (i + 1) & (hcap - 1); }
    hset[i] = s;
    ++hcount;
}

static int hhas(char *s) {
    int i;
    i = hash(s) & (hcap - 1);
    while (hset[i]) { if (same(hset[i], s)) return 1; i = (i + 1) & (hcap - 1); }
    return 0;
}

// ---- walking the tree ----

// What a walk found: each path, whether it is a directory, its stamp.
static char **wpath;
static int *wdir;
static int *wtime;
static int wn;
static int wcap;

static void found(char *path, int dir, int mtime) {
    char **np;
    int *nd, *nt;
    if (wn == wcap) {
        wcap = wcap ? wcap * 2 : 1024;
        np = (char **)malloc(wcap * sizeof(int));
        nd = (int *)malloc(wcap * sizeof(int));
        nt = (int *)malloc(wcap * sizeof(int));
        if (wn) {
            memcpy((char *)np, (char *)wpath, wn * sizeof(int));
            memcpy((char *)nd, (char *)wdir, wn * sizeof(int));
            memcpy((char *)nt, (char *)wtime, wn * sizeof(int));
            free((char *)wpath); free((char *)wdir); free((char *)wtime);
        }
        wpath = np; wdir = nd; wtime = nt;
    }
    wpath[wn] = dup(path);
    wdir[wn] = dir;
    wtime[wn] = mtime;
    ++wn;
}

// Everything under DIR (a path without its trailing slash; "" is /)
static void walk(char *dir) {
    char name[NAMEMAX];
    char *path;
    int i, kind, len, st[4];
    path = (char *)malloc(PATHMAX);
    i = 0;
    while ((kind = ureaddir(dir[0] ? dir : "/", i, name)) >= 0) {
        ++i;
        len = ustrlen(dir) + 1 + ustrlen(name);
        if (len >= PATHMAX) continue;
        ustrcpy(ustrcpy(ustrcpy(path, dir) - 1, "/") - 1, name);
        if (excluded(path)) continue;
        if (ustat(path, st) < 0) continue;
        found(path, kind == 1, st[3]);
        if (kind == 1) walk(path);
    }
    free(path);
}

static void walk_all() {
    wn = 0;
    walk("");
}

// ---- boot ----

// The base: every path there is now, one a line, and its stamp is the epoch
static int write_base() {
    int fd, i;
    umkdir("/var");
    umkdir("/var/persist");
    if ((fd = uopen(BASE, O_WR + O_CREATE + O_TRUNCATE)) < 0) return 0 - 1;
    i = 0;
    while (i < wn) {
        write(fd, wpath[i], ustrlen(wpath[i]));
        write(fd, "\n", 1);
        ++i;
    }
    uclose(fd);
    return 0;
}

// Lay the saved changes over the tree, then delete what they say was deleted
static int restore() {
    char *snap, *list, *p, *q;
    int len, slen, files, gone, fd;
    // nothing saved yet, or a save of nothing (forget): nothing to do
    if ((fd = uopen(SNAP, O_RD)) < 0) return 0;
    uclose(fd);
    if (!(snap = arc_readfile(SNAP, &slen))) return 0;
    if (slen < BLOCK || !snap[0]) { free(snap); return 0; }
    files = arc_untar(snap, slen, "/", ARC_EXTRACT, 0);
    free(snap);
    if (files < 0) {
        ufprintf(STDERR, "persist: %s: %s; starting clean\n", SNAP, arc_error ? arc_error : "unreadable");
        return 0 - 1;
    }
    gone = 0;
    if ((list = arc_readfile(WHITE, &len))) {
        p = list;
        while (p < list + len) {
            q = p;
            while (q < list + len && *q != '\n') ++q;
            *q = 0;
            if (*p && uunlink(p) == 0) ++gone;
            p = q + 1;
        }
        free(list);
    }
    uprintf("persist: %d files kept from the last session, %d deleted\n", list ? files - 1 : files, gone);
    return 0;
}

// ---- the unpacked system, kept ----

// The archive's id: its NAME.id's first line, else "size N"; 0 if neither
static char *archive_id(char *name) {
    char *idname, *id, *s;
    int n, len, st[4];
    n = ustrlen(name);
    idname = (char *)ualloc(n + 4);
    memcpy(idname, name, n);
    memcpy(idname + n, ".id", 4);
    id = arc_readfile(idname, &len);
    free(idname);
    if (id) {
        n = 0;
        while (n < len && id[n] != '\n' && id[n] != '\r') ++n;
        id[n] = 0;
        if (n > 0) return id;
        free(id);
    }
    if (ustat(name, st) < 0 || st[1] <= 0) return 0;
    id = (char *)ualloc(32);
    memcpy(id, "size ", 5);
    s = id + 5;
    n = st[1];
    len = 1;
    while (n >= 10) { n = n / 10; ++len; }
    s[len] = 0;
    n = st[1];
    while (len > 0) { --len; s[len] = '0' + n % 10; n = n / 10; }
    return id;
}

// The kept copy, if it is ID's: its files, or -1
static int from_kept(char *id) {
    char *have, *tar;
    int len, n, fd;
    if ((fd = uopen(KID, O_RD)) < 0) return 0 - 1;
    uclose(fd);
    if (!(have = arc_readfile(KID, &len))) return 0 - 1;
    have[len] = 0;
    n = same(have, id);
    free(have);
    if (!n) return 0 - 1;
    if (!(tar = arc_readfile(KTAR, &len))) return 0 - 1;
    n = arc_untar(tar, len, "/", ARC_EXTRACT, 0);
    free(tar);
    return n;
}

// Keep TAR, the archive ID unpacked to. The id is emptied first and
// written last, so a copy cut short is never taken for a whole one.
static void keep(char *tar, int len, char *id) {
    if (w_open(KID) < 0) return;
    if (w_close() < 0) return;
    if (w_open(KTAR) < 0) return;
    w_put(tar, len);
    if (w_close() < 0) { ufprintf(STDERR, "persist: could not keep %s\n", KTAR); return; }
    if (w_open(KID) < 0) return;
    w_put(id, ustrlen(id));
    w_close();
}

// The archive unpacked, as arc_extract does, and its tar kept for ID
static int unpack_keep(char *name, char *id) {
    char *raw, *tar;
    int rawlen, len, n;
    if (!(raw = arc_readfile(name, &rawlen))) return 0 - 1;
    if (rawlen >= 2 && (raw[0] & 255) == 0x1F && (raw[1] & 255) == 0x8B) {
        if (arc_gunzip(raw, rawlen, &tar, &len) < 0) { free(raw); return 0 - 1; }
        free(raw);
    } else {
        tar = raw;
        len = rawlen;
    }
    n = arc_untar(tar, len, "/", ARC_EXTRACT, 0);
    if (n >= 0 && id) keep(tar, len, id);
    free(tar);
    return n;
}

static int boot(int argc, char **argv) {
    int i, n;
    char *id;
    i = 2;
    // the root archive alone, and a drive: the copy kept from the boot
    // before, or the archive unpacked and its copy kept for the next
    if (argc == 3 && have_drive()) {
        id = archive_id(argv[2]);
        n = id ? from_kept(id) : 0 - 1;
        if (n >= 0) uprintf("persist: %s: %d files from the copy kept on drive 1\n", argv[2], n);
        else if (unpack_keep(argv[2], id) < 0) ufprintf(STDERR, "persist: %s: %s\n", argv[2], arc_error ? arc_error : "failed");
        if (id) free(id);
        i = argc;
    }
    while (i < argc) {
        n = arc_extract(argv[i], "/", ARC_EXTRACT, 0);
        if (n < 0) ufprintf(STDERR, "persist: %s: %s\n", argv[i], arc_error ? arc_error : "failed");
        ++i;
    }
    if (!have_drive()) return 0;
    walk_all();
    if (write_base() < 0) { ufprintf(STDERR, "persist: cannot write %s\n", BASE); return 1; }
    restore();
    return 0;
}

// ---- save ----

static int epoch;

// the base and its epoch into hset; 0 if there is none (no boot noted it)
static int load_base() {
    char *buf, *p, *q;
    int len, n, st[4];
    if (ustat(BASE, st) < 0) return 0;
    epoch = st[3];
    if (!(buf = arc_readfile(BASE, &len))) return 0;
    n = 0;
    p = buf;
    while (p < buf + len) { if (*p == '\n') ++n; ++p; }
    hinit(n);
    p = buf;
    while (p < buf + len) {
        q = p;
        while (q < buf + len && *q != '\n') ++q;
        *q = 0;
        if (*p) hadd(p);
        p = q + 1;
    }
    return 1;
}

static char *hdr;
static char *chunk;

static void octal(char *at, int width, int v) {
    int i;
    i = width - 1;
    at[i] = 0;
    while (i > 0) { --i; at[i] = '0' + (v & 7); v = (v >> 3) & 0x1fffffff; }
}

// A ustar header for PATH (absolute; stored without its leading slash).
// 0 if the name will not fit ustar's 100 + 155.
static int header(char *path, int size, int dir) {
    char *name;
    int n, cut, i, sum;
    name = path + 1;
    n = ustrlen(name);
    memset(hdr, 0, BLOCK);
    if (n <= 100) memcpy(hdr, name, n);
    else {
        // the prefix ends at a slash, and what follows fits in 100
        cut = n - 101;
        while (cut < n && name[cut] != '/') ++cut;
        if (cut >= n || cut > 155) return 0;
        memcpy(hdr + 345, name, cut);
        memcpy(hdr, name + cut + 1, n - cut - 1);
    }
    octal(hdr + 100, 8, dir ? 493 : 420);       // 0755, 0644
    octal(hdr + 108, 8, 0);
    octal(hdr + 116, 8, 0);
    octal(hdr + 124, 12, dir ? 0 : size);
    octal(hdr + 136, 12, 0);
    hdr[156] = dir ? '5' : '0';
    memcpy(hdr + 257, "ustar", 6);
    hdr[263] = '0'; hdr[264] = '0';
    memset(hdr + 148, ' ', 8);
    sum = 0;
    i = 0;
    while (i < BLOCK) { sum = sum + (hdr[i] & 255); ++i; }
    octal(hdr + 148, 7, sum);
    hdr[155] = ' ';
    return 1;
}

static void pad(int size) {
    int r;
    r = size % BLOCK;
    if (r) { memset(chunk, 0, BLOCK - r); w_put(chunk, BLOCK - r); }
}

// one file's header and bytes; 0 if it could not be kept
static int put_file(char *path) {
    int fd, n, got, size, st[4];
    if (ustat(path, st) < 0) return 0;
    size = st[1];
    if (!header(path, size, 0)) { ufprintf(STDERR, "persist: %s: name too long to keep\n", path); return 0; }
    if ((fd = uopen(path, O_RD)) < 0) return 0;
    w_put(hdr, BLOCK);
    n = 0;
    while (n < size) {
        got = uread(fd, chunk, size - n < CHUNK ? size - n : CHUNK);
        if (got <= 0) break;
        w_put(chunk, got);
        n = n + got;
    }
    // a file that shrank as it was read: the header promised SIZE bytes
    if (n < size) { memset(chunk, 0, CHUNK); while (n < size) { w_put(chunk, size - n < CHUNK ? size - n : CHUNK); n = n + CHUNK; } }
    uclose(fd);
    pad(size);
    return 1;
}

static int *order;       // indices of the changes, in the order they were made
static int norder;
static int nwhite;
static int wbytes;       // the whiteout list's length
static int sig;          // a summary of what would be written, for -d

static int changed(int i) {
    if (!hhas(wpath[i])) return 1;
    return !wdir[i] && wtime[i] > epoch;
}

// What a save would write: the changes in order, the deletions, and SIG
static void survey() {
    int i, j, k, t, st[4];
    walk_all();
    order = (int *)malloc((wn + 1) * sizeof(int));
    norder = 0;
    sig = 0;
    i = 0;
    while (i < wn) {
        if (changed(i)) { order[norder++] = i; sig = sig * 31 + hash(wpath[i]) + wtime[i]; }
        ++i;
    }
    // insertion sort by stamp, directories (stamped or not) first
    i = 1;
    while (i < norder) {
        k = order[i];
        t = wdir[k] ? 0 - 1 : wtime[k];
        j = i - 1;
        while (j >= 0 && (wdir[order[j]] ? 0 - 1 : wtime[order[j]]) > t) { order[j + 1] = order[j]; --j; }
        order[j + 1] = k;
        ++i;
    }
    // deletions: base paths that are gone
    nwhite = 0; wbytes = 0;
    i = 0;
    while (i < hcap) {
        if (hset[i] && !excluded(hset[i]) && ustat(hset[i], st) < 0) {
            ++nwhite;
            wbytes = wbytes + ustrlen(hset[i]) + 1;
            sig = sig * 31 + hash(hset[i]);
        }
        ++i;
    }
}

static int save(int quiet) {
    int i, k, files, at, st[4];
    char *list;
    files = 0;
    if (w_open(SNAP) < 0) { ufprintf(STDERR, "persist: cannot write %s\n", SNAP); return 1; }
    i = 0;
    while (i < norder) {
        k = order[i];
        if (wdir[k]) {
            if (header(wpath[k], 0, 1)) { w_put(hdr, BLOCK); ++files; }
        } else files = files + put_file(wpath[k]);
        ++i;
    }
    if (nwhite) {
        list = (char *)malloc(wbytes + 1);
        at = 0;
        i = 0;
        while (i < hcap) {
            if (hset[i] && !excluded(hset[i]) && ustat(hset[i], st) < 0) {
                ustrcpy(list + at, hset[i]);
                at = at + ustrlen(hset[i]);
                list[at++] = '\n';
            }
            ++i;
        }
        header(WHITE, at, 0);
        w_put(hdr, BLOCK);
        w_put(list, at);
        pad(at);
        free(list);
    }
    // the end of the archive: two zero blocks
    memset(chunk, 0, BLOCK * 2);
    w_put(chunk, BLOCK * 2);
    if (w_close() < 0) { ufprintf(STDERR, "persist: writing %s failed\n", SNAP); return 1; }
    if (!quiet) uprintf("persist: %d changes and %d deletions kept (%d KB)\n", files, nwhite, (wtotal + 1023) / 1024);
    return 0;
}

static void freewalk() {
    int i;
    i = 0;
    while (i < wn) { free(wpath[i]); ++i; }
    wn = 0;
    if (order) { free((char *)order); order = 0; }
}

static int usage() {
    ufprintf(STDERR, "usage: persist boot ARCHIVE... | save | status | forget | -d SECS\n");
    return 2;
}

int main(int argc, char **argv) {
    char *me;
    int i, secs, last, rc;

    hdr = (char *)malloc(BLOCK);
    chunk = (char *)malloc(CHUNK);
    // the drive's registers: under the MMU only for a task that asks
    if (__c4_info() & C4I_MMU) uioperm(UIOPERM_DEV);
    me = argv[0];
    i = 0;
    while (argv[0][i]) { if (argv[0][i] == '/') me = argv[0] + i + 1; ++i; }
    if (argc > 1 && same(argv[1], "boot")) return boot(argc, argv);

    // the daemon is started on every desktop boot: where there is no
    // drive it simply has nothing to do
    if (!have_drive() || !load_base()) {
        if (argc > 1 && same(argv[1], "-d")) return 0;
        if (!have_drive()) ufprintf(STDERR, "persist: no drive to keep things on\n");
        else ufprintf(STDERR, "persist: this boot noted nothing to compare with\n");
        return 1;
    }

    if (starts(me, "sync") || (argc > 1 && same(argv[1], "save"))) {
        survey();
        return save(0);
    }
    if (argc > 1 && same(argv[1], "status")) {
        survey();
        uprintf("persist: %d changes and %d deletions since boot\n", norder, nwhite);
        i = 0;
        while (i < norder) { uprintf("  %s%s\n", wpath[order[i]], wdir[order[i]] ? "/" : ""); ++i; }
        return 0;
    }
    if (argc > 1 && same(argv[1], "forget")) {
        // an archive of nothing: the next boot is the archive alone
        if (w_open(SNAP) < 0) return 1;
        memset(chunk, 0, BLOCK * 2);
        w_put(chunk, BLOCK * 2);
        if (w_close() < 0) return 1;
        uprintf("persist: forgotten; the next boot starts clean\n");
        return 0;
    }
    if (argc > 2 && same(argv[1], "-d")) {
        secs = 0;
        i = 0;
        while (argv[2][i] >= '0' && argv[2][i] <= '9') { secs = secs * 10 + argv[2][i] - '0'; ++i; }
        if (secs < 1) return usage();
        survey();
        last = sig;
        freewalk();
        for (;;) {
            umsleep(secs * 1000);
            survey();
            if (sig != last) {
                rc = save(1);
                if (rc == 0) last = sig;
            }
            freewalk();
        }
    }
    return usage();
}

// C4IX userland: persist -- the filesystem's changes, kept on a drive
// that outlives the machine (docs/c4ix-design.md, "Persistence").
//
//   persist boot ARCHIVE...  unpack the system (vfsload runs this as the
//                            boot's unpacking task), note what it is,
//                            then lay the saved changes over it; the
//                            unpacked system is kept on the drive, its
//                            files are read from there, and the next
//                            boot reads that copy instead
//   persist save             write every change since boot to the drive
//   sync                     the same (/bin/sync is this program)
//   persist status           what save would write
//   persist forget           save nothing: the next boot starts clean
//   persist -d SECS          save every SECS seconds while things change
//
// C4IX's files are RAM. What lasts is host drive 1 -- in the browser an
// IndexedDB store the page keeps (libjs/page.js), on the command line
// the directory `cli.js -w` names. The changes go there as one tar,
// written through c4bb's disk write registers and made whole when it
// closes: 1:c4ix-persist.tar or 1:c4ix-persist-b.tar, each save into the
// one the last did not use, and then 1:c4ix-persist.cur names it ("a"
// or "b"; none: a drive from before there were two, its one tar). A save
// cut short leaves the last.
//
// What is saved is the drive's again. Once the tar is whole and named,
// each file saved is read from its span of it (urelazy), as the kept
// system's files are (below): a big one's RAM is freed then, a small one
// is kept as a cache memory can take back. The next boot makes the
// saved files spans of the tar too, and untars nothing into RAM. A
// changed file is RAM's only until it is saved. Why two tars: the files
// read from the last are read from it until this save has made them
// this one's, and a tar is replaced at once when written (devices.js).
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
// /tmp, /proc, /var/log and /var/persist are never kept.
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
//
// Read where it is kept. The kept tar is not untarred into RAM: each of
// its files is made a lazy file (ulazyfile) whose bytes are its span of
// the tar on drive 1, "1:c4ix-base.tar@OFF+LEN", which the host reads
// (devices.js diskOpen). Nothing of them is read at boot; the kernel
// reads a file from there when it is wanted, keeps a small one while
// memory allows and drops it again when memory is short (vfs.c, the
// cache), and a file that changes is RAM's until persist save keeps it,
// and then the drive's again.
// The first boot inflates the archive straight onto the drive, a window
// at a time (arc_gunzip_to), and then does the same, so no boot holds
// the unpacked tree in RAM. Where there is no drive, or it will not take
// the tar, the archive is unpacked into RAM as before.
//
// Out of memory. A boot that cannot unpack the system says so and exits
// 3 (NOMEM); vfsload and init then stop the desktop's boot there, where
// the desktop on half a filesystem would only fail later, less clearly.
// A root archive that cannot be read for any other reason (damaged: "bad
// literal/length code", "CRC mismatch", "not a tar archive") is 4
// (DAMAGED), and stops the desktop's boot too: a desktop with no tree
// under it looks like one, and then every program on it fails.

#include "c4ix_user.h"
#include "archive.h"

enum {
    C4I_C4MJS = 8, C4I_MMU = 0x80000,
    DISK_FD = 0x124, DISK_WNAME = 0x178, DISK_WADDR = 0x17c, DISK_WLEN = 0x180,
    DISK_WCLOSE = 0x184, DISK_COUNT = 0x188,
    HEAP_BASE = 0x144, HEAP_END = 0x148,
    PATHMAX = 256, NAMEMAX = 64, CHUNK = 65536, BLOCK = 512,
    NOMEM = 3,             // boot's exit status when memory ran out
    DAMAGED = 4            // ... and when the root archive could not be read
};

static char *SNAP = "1:c4ix-persist.tar";      // the changes saved: this one or SNAP2, as CUR says
static char *SNAP2 = "1:c4ix-persist-b.tar";
static char *CUR = "1:c4ix-persist.cur";
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

// ---- the snapshots ----

static int exists(char *name) {
    int fd;
    if ((fd = uopen(name, O_RD)) < 0) return 0;
    uclose(fd);
    return 1;
}

// The snapshot the last save finished, or 0 for none yet
static char *snap_cur() {
    char *c;
    int len, b;
    if (exists(CUR) && (c = arc_readfile(CUR, &len))) {
        b = len > 0 && c[0] == 'b';
        free(c);
        return b ? SNAP2 : SNAP;
    }
    return exists(SNAP) ? SNAP : 0;
}

// the one a save writes: not the one files may be read from now
static char *snap_next() {
    return snap_cur() == SNAP ? SNAP2 : SNAP;
}

// TO, whole, is the one the next boot reads
static int snap_name(char *to) {
    w_open(CUR);
    w_put(to == SNAP2 ? "b" : "a", 1);
    return w_close();
}

// ---- one save at a time ----
//
// Two saves at once -- the desktop's persist -d and a sync typed -- would
// write the same tar, each a register at a time, into each other, and the
// one that closed second would leave the other's files read from its
// tar. A directory made is the lock: making it fails if it is there
// (vfs_mkdir, under the kernel's lock). The holder's task id is in it,
// so a holder that is gone, killed mid-save, leaves it to the next.

static char *LOCK = "/tmp/persist.lock";
static char *LOCKID = "/tmp/persist.lock/id";

// the holder's task id; 0 if it has not written it yet
static int lock_holder() {
    char *p;
    int len, i, id;
    if (!(p = arc_readfile(LOCKID, &len))) return 0;
    id = 0;
    i = 0;
    while (i < len && p[i] >= '0' && p[i] <= '9') { id = id * 10 + p[i] - '0'; ++i; }
    free(p);
    return id;
}

// The lock, waiting for it if WAIT (up to five minutes); 0 if not had
static int lock_take(int wait) {
    char id[16];
    int fd, holder, tries, n, v;
    tries = 0;
    while (umkdir(LOCK) < 0) {
        holder = lock_holder();
        // a holder no longer there (UNICE_NONE: no such task), or one that
        // made it and never named itself in five seconds
        if ((holder > 0 && unice(holder, UNICE_ASK) == UNICE_NONE) || (holder == 0 && tries >= 50)) {
            uunlink(LOCKID);
            uunlink(LOCK);
            tries = 0;
            continue;
        }
        if (holder > 0 && !wait) return 0;
        if (++tries > 3000) return 0;
        umsleep(100);
    }
    n = 15;
    id[n] = 0;
    v = getpid();
    while (n > 0) { id[--n] = '0' + v % 10; v = v / 10; if (!v) break; }
    if ((fd = uopen(LOCKID, O_WR + O_CREATE + O_TRUNCATE)) >= 0) { write(fd, id + n, 15 - n); uclose(fd); }
    return 1;
}

static void lock_drop() {
    uunlink(LOCKID);
    uunlink(LOCK);
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
           same(path, "/var/log") || starts(path, "/var/log/") ||
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

// Everything under DIR (a path without its trailing slash; "" is /). A
// directory whose entries are still the host's (stat's device 3: vfs.c,
// vfs_lazydir) is found, as a 2, but not gone into: that would make them.
static void walk(char *dir) {
    char name[NAMEMAX];
    char *path;
    int i, kind, len, st[ST_WORDS];
    path = (char *)malloc(PATHMAX);
    i = 0;
    while ((kind = ureaddir(dir[0] ? dir : "/", i, name)) >= 0) {
        ++i;
        len = ustrlen(dir) + 1 + ustrlen(name);
        if (len >= PATHMAX) continue;
        ustrcpy(ustrcpy(ustrcpy(path, dir) - 1, "/") - 1, name);
        if (excluded(path)) continue;
        if (ustat2(path, st, ST_WORDS) < 0) continue;
        if (kind == 1 && st[ST_DEV] == 3) { found(path, 2, st[3]); continue; }
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

// The base: every path there is now, one a line, and its stamp is the
// epoch. A host directory is there twice, the second time as "PATH/":
// what it holds was not there to walk, and is not a change unless it is
// stamped after the epoch (changed).
static int write_base() {
    int fd, i;
    umkdir("/var");
    umkdir("/var/persist");
    if ((fd = uopen(BASE, O_WR + O_CREATE + O_TRUNCATE)) < 0) return 0 - 1;
    i = 0;
    while (i < wn) {
        write(fd, wpath[i], ustrlen(wpath[i]));
        write(fd, "\n", 1);
        if (wdir[i] == 2) { write(fd, wpath[i], ustrlen(wpath[i])); write(fd, "/\n", 2); }
        ++i;
    }
    uclose(fd);
    return 0;
}

static int nomem;          // what failed was memory
static int lazy_tar(char *tar);

// Lay the saved changes over the tree, as spans of the snapshot they are
// in, then delete what they say was deleted
static int restore() {
    char *snap, *list, *p, *q;
    int len, files, gone;
    // nothing saved yet: nothing to do (a save of nothing, forget's, has no files)
    if (!(snap = snap_cur())) return 0;
    // its files read where they are, on their spans of it (lazy_tar)
    nomem = 0;
    files = lazy_tar(snap);
    if (files < 0) {
        ufprintf(STDERR, "persist: %s: %s; starting clean\n", snap, nomem ? "out of memory" : "unreadable");
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
        // read: a span of a tar a later save replaces (it is never saved)
        uunlink(WHITE);
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

// ---- the kept tar, read where it is ----

static int oct(char *p, int n) {
    int v, i;
    v = 0; i = 0;
    while (i < n && (p[i] == ' ' || !p[i])) ++i;
    while (i < n && p[i] >= '0' && p[i] <= '7') { v = v * 8 + p[i] - '0'; ++i; }
    return v;
}

// S gets V in decimal; the end of what it got
static char *putdec(char *s, int v) {
    char d[12];
    int n;
    n = 0;
    d[n++] = '0' + v % 10; v = v / 10;
    while (v > 0) { d[n++] = '0' + v % 10; v = v / 10; }
    while (n) *s++ = d[--n];
    *s = 0;
    return s;
}

// OUT gets the name of LEN bytes of TAR from POS: TAR@POS+LEN
static void span(char *out, char *tar, int pos, int len) {
    char *e;
    e = ustrcpy(out, tar) - 1;
    *e++ = '@';
    e = putdec(e, pos);
    *e++ = '+';
    putdec(e, len);
}

// TAR's bytes at POS, LEN of them, into BUF; how many
static int span_read(char *tar, int pos, int len, char *buf) {
    char name[PATHMAX];
    int fd, n, got;
    span(name, tar, pos, len);
    if ((fd = uopen(name, O_RD)) < 0) return 0 - 1;
    got = 0;
    while (got < len && (n = uread(fd, buf + got, len - got)) > 0) got = got + n;
    uclose(fd);
    return got;
}

// The tar on the drive, its files made lazy files on their spans of it,
// in its order, and its directories made: the files, or -1 (nomem says
// whether memory ran out). A tar this does not read (not ustar) is -1
// before anything is made.
static int lazy_tar(char *tar) {
    char name[PATHMAX], path[PATHMAX], dir[PATHMAX], host[PATHMAX];
    char *e;
    int pos, size, type, files, i, n, o, slash;
    pos = 0; files = 0; dir[0] = 0;
    while (span_read(tar, pos, BLOCK, hdr) == BLOCK && hdr[0]) {
        if (hdr[257] != 'u' || hdr[258] != 's' || hdr[259] != 't') return 0 - 1;
        // ustar: PREFIX "/" NAME, where there is a prefix
        n = 0;
        if (hdr[345]) { i = 0; while (i < 155 && hdr[345 + i]) name[n++] = hdr[345 + i++]; name[n++] = '/'; }
        i = 0; while (i < 100 && hdr[i]) name[n++] = hdr[i++];
        name[n] = 0;
        size = oct(hdr + 124, 12);
        type = hdr[156];
        pos = pos + BLOCK;
        // "/" NAME, without "./" before it or "/" after it
        e = name;
        while (e[0] == '.' && e[1] == '/') e = e + 2;
        while (e[0] == '/') ++e;
        path[0] = '/';
        o = 1;
        while (*e && o < PATHMAX - 1) path[o++] = *e++;
        while (o > 1 && path[o - 1] == '/') --o;
        path[o] = 0;
        if (type == '5') {
            if (arc_mkdirs(path) < 0) { nomem = 1; return 0 - 1; }
        } else if (type == '0' || type == 0 || type == '7') {
            // its directory, once for a run of files in it
            slash = o - 1;
            while (slash > 0 && path[slash] != '/') --slash;
            path[slash] = 0;
            if (slash > 0 && !same(dir, path)) {
                if (arc_mkdirs(path) < 0) { nomem = 1; return 0 - 1; }
                ustrcpy(dir, path);
            }
            path[slash] = '/';
            span(host, tar, pos, size);
            if (ulazyfile(path, host, size) < 0) { nomem = 1; return 0 - 1; }
            ++files;
        }
        // links, devices and pax headers have nothing C4IX can hold
        pos = pos + (size + BLOCK - 1) / BLOCK * BLOCK;
    }
    return files;
}

// The kept copy, if it is ID's: its files, or -1
static int from_kept(char *id) {
    char *have;
    int len, n, fd;
    if ((fd = uopen(KID, O_RD)) < 0) return 0 - 1;
    uclose(fd);
    if (!(have = arc_readfile(KID, &len))) return 0 - 1;
    have[len] = 0;
    n = same(have, id);
    free(have);
    if (!n) return 0 - 1;
    return lazy_tar(KTAR);
}

// arc_gunzip_to's sink: the tar onto the drive as it is inflated
static int to_drive(char *buf, int len) {
    w_put(buf, len);
    return wbad ? 0 - 1 : len;
}

// was the last archive failure memory? (libarc's messages for it start "out of memory")
static int arc_nomem() {
    return !arc_error || (arc_error[0] == 'o' && arc_error[1] == 'u');
}

// The archive (RAW, read in) unpacked into RAM, as arc_extract does; its
// tar kept for ID too, where there is one. The way where there is no
// drive that takes the tar.
static int unpack_ram(char *raw, int rawlen, char *id) {
    char *tar;
    int len, n;
    if (rawlen >= 2 && (raw[0] & 255) == 0x1F && (raw[1] & 255) == 0x8B) {
        if (arc_gunzip(raw, rawlen, &tar, &len) < 0) { free(raw); nomem = arc_nomem(); return 0 - 1; }
        free(raw);
    } else {
        tar = raw;
        len = rawlen;
    }
    n = arc_untar(tar, len, "/", ARC_EXTRACT, 0);
    // a file that could not be written: there is nothing else it can be
    if (n < 0 && !arc_error) { nomem = 1; arc_error = "out of memory for its files"; }
    if (n >= 0 && id && w_open(KID) >= 0 && w_close() >= 0 && w_open(KTAR) >= 0) {
        w_put(tar, len);
        if (w_close() >= 0 && w_open(KID) >= 0) { w_put(id, ustrlen(id)); w_close(); }
    }
    free(tar);
    return n;
}

// The archive inflated onto the drive as the tar ID unpacked to, and its
// files read from there (lazy_tar). The id is emptied first and written
// last, so a copy cut short is never taken for a whole one. Where the
// drive will not take it, unpacked into RAM instead.
//
// The archive is inflated as it is read (arc_gunzip_file), 128 KB of it
// at a time: read whole first, its 4.3 MB were in memory beside the
// desktop starting, and a first boot without the MMU needed 10 MB.
static int unpack_keep(char *name, char *id) {
    char *raw;
    int rawlen, n, kept;
    kept = 0;
    if (id && w_open(KID) >= 0 && w_close() >= 0 && w_open(KTAR) >= 0) {
        n = arc_gunzip_file(name, (int)&to_drive);
        if (w_close() < 0 || wbad) {
            ufprintf(STDERR, "persist: could not keep %s; unpacking into memory\n", KTAR);
            id = 0;
        } else if (n != ARC_NOTGZ) kept = 1;
    }
    if (!kept) {
        // a plain tar, or a drive that will not take it: into RAM
        if (!(raw = arc_readfile(name, &rawlen))) { nomem = arc_nomem(); return 0 - 1; }
        return unpack_ram(raw, rawlen, id);
    }
    if (n < 0) { nomem = arc_nomem(); return 0 - 1; }
    if (w_open(KID) >= 0) { w_put(id, ustrlen(id)); w_close(); }
    if ((n = lazy_tar(KTAR)) >= 0) uprintf("persist: %s: %d files, unpacked onto drive 1 and read from there\n", name, n);
    return n;
}

// How much memory this machine has for programs, in MB, where the host
// says: its heap, as the kernel's boot line counts it
static int heap_mb() {
    if (!(__c4_info() & C4I_C4MJS)) return 0;
    return (*(int *)HEAP_END - *(int *)HEAP_BASE) / 1048576;
}

// NAME could not be unpacked: why; NOMEM if it was memory, else DAMAGED
// if it is the root archive (ROOT), the system's own files
static int failed(char *name, int root) {
    ufprintf(STDERR, "persist: %s: %s\n", name, arc_error ? arc_error : nomem ? "out of memory" : "failed");
    if (!nomem) return root ? DAMAGED : 0;
    if (heap_mb()) ufprintf(STDERR, "persist: out of memory: %s does not fit in this machine's %d MB of heap\n", name, heap_mb());
    else ufprintf(STDERR, "persist: out of memory unpacking %s\n", name);
    return NOMEM;
}

static int boot(int argc, char **argv) {
    int i, n, rc;
    char *id;
    i = 2;
    rc = 0;
    // the root archive alone, and a drive: the copy kept from the boot
    // before, or the archive inflated onto the drive for this boot and
    // the next; either way its files are read from there
    if (argc == 3 && have_drive()) {
        id = archive_id(argv[2]);
        nomem = 0;
        arc_error = 0;
        n = id ? from_kept(id) : 0 - 1;
        if (n >= 0) uprintf("persist: %s: %d files from the copy kept on drive 1, read from there\n", argv[2], n);
        else if (nomem) rc = failed(argv[2], 1);
        else if ((n = unpack_keep(argv[2], id)) < 0) rc = failed(argv[2], 1);
        if (id) free(id);
        if (rc) return rc;
        i = argc;
    }
    while (i < argc) {
        nomem = 0;
        n = arc_extract(argv[i], "/", ARC_EXTRACT, 0);
        if (n < 0) {
            if (!arc_error) nomem = 1;
            if ((rc = failed(argv[i], i == 2))) return rc;
        }
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

static int *soff;        // where each change's bytes went in the tar, and how many; -1 if not
static int *ssize;

// one file's header and bytes, the change K; 0 if it could not be kept
static int put_file(int k) {
    char *path;
    int fd, n, got, size, st[4];
    path = wpath[k];
    if (ustat(path, st) < 0) return 0;
    size = st[1];
    if (!header(path, size, 0)) { ufprintf(STDERR, "persist: %s: name too long to keep\n", path); return 0; }
    if ((fd = uopen(path, O_RD)) < 0) return 0;
    w_put(hdr, BLOCK);
    soff[k] = wtotal;
    ssize[k] = size;
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

// Is PATH inside a directory that was the host's when the base was
// noted ("DIR/" in it)?
static int in_hostdir(char *path) {
    char *p;
    int n, c;
    n = ustrlen(path);
    while (n > 1) {
        --n;
        if (path[n] != '/') continue;
        p = path + n + 1;
        c = *p;
        *p = 0;
        if (hhas(path)) { *p = c; return 1; }
        *p = c;
    }
    return 0;
}

static int changed(int i) {
    // the host's, made since: a change only if stamped after the epoch
    if (!hhas(wpath[i])) return !in_hostdir(wpath[i]) || wtime[i] > epoch;
    return wdir[i] != 1 && wtime[i] > epoch;
}

// A base path that is gone: a deletion. A host directory's "DIR/" line
// is not a path of its own (write_base).
static int gone(char *p) {
    int st[4];
    return p && !excluded(p) && p[ustrlen(p) - 1] != '/' && ustat(p, st) < 0;
}

// What a save would write: the changes in order, the deletions, and SIG
static void survey() {
    int i, j, k, t, st[4];
    walk_all();
    order = (int *)malloc((wn + 1) * sizeof(int));
    soff = (int *)malloc((wn + 1) * sizeof(int));
    ssize = (int *)malloc((wn + 1) * sizeof(int));
    norder = 0;
    sig = 0;
    i = 0;
    while (i < wn) {
        soff[i] = 0 - 1;
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
        if (gone(hset[i])) {
            ++nwhite;
            wbytes = wbytes + ustrlen(hset[i]) + 1;
            sig = sig * 31 + hash(hset[i]);
        }
        ++i;
    }
}

static int save(int quiet) {
    int i, k, files, at, kb, st[4];
    char *list, *to;
    char host[PATHMAX];
    files = 0;
    to = snap_next();
    if (w_open(to) < 0) { ufprintf(STDERR, "persist: cannot write %s\n", to); return 1; }
    i = 0;
    while (i < norder) {
        k = order[i];
        if (wdir[k]) {
            if (header(wpath[k], 0, 1)) { w_put(hdr, BLOCK); ++files; }
        } else files = files + put_file(k);
        ++i;
    }
    if (nwhite) {
        list = (char *)malloc(wbytes + 1);
        at = 0;
        i = 0;
        while (i < hcap) {
            if (gone(hset[i])) {
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
    if (w_close() < 0) { ufprintf(STDERR, "persist: writing %s failed\n", to); return 1; }
    kb = (wtotal + 1023) / 1024;
    if (snap_name(to) < 0) { ufprintf(STDERR, "persist: writing %s failed\n", CUR); return 1; }
    // what was saved is the drive's again, but what changed since the
    // survey (its bytes newer than these)
    i = 0;
    while (i < norder) {
        k = order[i];
        if (!wdir[k] && soff[k] >= 0) {
            span(host, to, soff[k], ssize[k]);
            urelazy(wpath[k], host, wtime[k]);
        }
        ++i;
    }
    if (!quiet) uprintf("persist: %d changes and %d deletions kept (%d KB)\n", files, nwhite, kb);
    return 0;
}

static void freewalk() {
    int i;
    i = 0;
    while (i < wn) { free(wpath[i]); ++i; }
    wn = 0;
    if (order) { free((char *)order); order = 0; }
    if (soff) { free((char *)soff); free((char *)ssize); soff = 0; ssize = 0; }
}

// ---- download: what the person takes away --------------------------------
//
//   download PATH...         each file handed to the web page, which saves
//   persist download PATH... it where the browser keeps downloads; a
//                            folder as NAME.tar, all that is in it (ustar,
//                            as save writes)
//
// The host takes it as a file written to "download:NAME" through the same
// write registers (libjs's devices.js, onDownload), and keeps nothing of it
// on the machine's side. Where nothing takes it -- the command line, native
// c4m -- it says so. download is persist by another name (/bin/download,
// an ilink, as sync is), and the desktop's Files runs it for File >
// Download (desktop/files.c). A file in the page's store (drive 1) can also
// be saved from the page itself, without the machine (libjs/README.md).

static int dl_open(char *name) {
    char full[PATHMAX + 16];
    ustrcpy(ustrcpy(full, "download:") - 1, name);
    return w_open(full);
}

// the file ABS into the tar as REL ("/NAME/...", as header wants), SIZE bytes
static int dl_file(char *abs, char *rel, int size) {
    int fd, n, got;
    if (!header(rel, size, 0)) { ufprintf(STDERR, "download: %s: name too long for a tar\n", abs); return 0; }
    if ((fd = uopen(abs, O_RD)) < 0) { ufprintf(STDERR, "download: cannot read %s\n", abs); return 0; }
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

// the folder ABS and all in it, as REL and what is under it; how many files
static int dl_tree(char *abs, char *rel) {
    char name[NAMEMAX];
    char *a, *r;
    int i, kind, n, st[4];
    a = (char *)malloc(PATHMAX);
    r = (char *)malloc(PATHMAX);
    n = 0;
    if (!a || !r) return 0;
    // the folder's own entry, "NAME/"
    if (ustrlen(rel) + 2 < PATHMAX) {
        ustrcpy(ustrcpy(r, rel) - 1, "/");
        if (header(r, 0, 1)) w_put(hdr, BLOCK);
    }
    i = 0;
    while ((kind = ureaddir(abs, i, name)) >= 0) {
        ++i;
        if (ustrlen(abs) + 2 + ustrlen(name) >= PATHMAX || ustrlen(rel) + 2 + ustrlen(name) >= PATHMAX) continue;
        ustrcpy(ustrcpy(ustrcpy(a, abs[1] ? abs : "") - 1, "/") - 1, name);
        ustrcpy(ustrcpy(ustrcpy(r, rel) - 1, "/") - 1, name);
        // what is made up as it is read is not taken away
        if (starts(a, "/proc/") || same(a, "/proc")) continue;
        if (ustat(a, st) < 0) continue;
        if (st[0] == 1) n = n + dl_tree(a, r);
        else if (st[0] == 2 && dl_file(a, r, st[1])) ++n;
    }
    free(a);
    free(r);
    return n;
}

static int download(int argc, char **argv, int from) {
    char path[PATHMAX], name[NAMEMAX + 8], rel[NAMEMAX + 2];
    char *p, *b;
    int i, k, n, rc, fd, got, st[4];
    if (from >= argc) { ufprintf(STDERR, "usage: download FILE|FOLDER...\n"); return 2; }
    if (!(__c4_info() & C4I_C4MJS)) { ufprintf(STDERR, "download: only the web page takes downloads\n"); return 1; }
    rc = 0;
    i = from;
    while (i < argc) {
        p = argv[i];
        ++i;
        if (ustrlen(p) >= PATHMAX) { rc = 1; continue; }
        ustrcpy(path, p);
        // "dir/" is "dir"; "/" stays
        k = ustrlen(path);
        while (k > 1 && path[k - 1] == '/') { --k; path[k] = 0; }
        if (ustat(path, st) < 0) { ufprintf(STDERR, "download: %s: no such file or folder\n", p); rc = 1; continue; }
        // the name it is saved as: its last part ("/" is c4ix)
        b = path;
        k = 0;
        while (path[k]) { if (path[k] == '/' && path[k + 1]) b = path + k + 1; ++k; }
        if (*b == '/' || !*b) b = "c4ix";
        if (ustrlen(b) > NAMEMAX) { ufprintf(STDERR, "download: %s: name too long\n", p); rc = 1; continue; }
        ustrcpy(name, b);
        if (st[0] == 1) {
            ustrcpy(name + ustrlen(name), ".tar");
            if (dl_open(name) < 0) { ufprintf(STDERR, "download: nothing here takes downloads (the web page does)\n"); return 1; }
            // inside it, "NAME/...", as "/NAME/..." for header
            ustrcpy(ustrcpy(rel, "/") - 1, b);
            n = dl_tree(path, rel);
            memset(chunk, 0, BLOCK * 2);
            w_put(chunk, BLOCK * 2);
            if (w_close() < 0) { ufprintf(STDERR, "download: %s: not taken\n", p); rc = 1; continue; }
            uprintf("download: %s, %d file%s\n", name, n, n == 1 ? "" : "s");
        } else {
            if ((fd = uopen(path, O_RD)) < 0) { ufprintf(STDERR, "download: cannot read %s\n", p); rc = 1; continue; }
            if (dl_open(name) < 0) { uclose(fd); ufprintf(STDERR, "download: nothing here takes downloads (the web page does)\n"); return 1; }
            n = 0;
            while ((got = uread(fd, chunk, CHUNK)) > 0) { w_put(chunk, got); n = n + got; }
            uclose(fd);
            if (w_close() < 0) { ufprintf(STDERR, "download: %s: not taken\n", p); rc = 1; continue; }
            uprintf("download: %s, %d bytes\n", name, n);
        }
    }
    return rc;
}

static int usage() {
    ufprintf(STDERR, "usage: persist boot ARCHIVE... | save | status | forget | -d SECS | download PATH...\n");
    return 2;
}

int main(int argc, char **argv) {
    char *me, *to;
    int i, secs, last, rc;

    hdr = (char *)malloc(BLOCK);
    chunk = (char *)malloc(CHUNK);
    // the drive's registers: under the MMU only for a task that asks
    if (__c4_info() & C4I_MMU) uioperm(UIOPERM_DEV);
    me = argv[0];
    i = 0;
    while (argv[0][i]) { if (argv[0][i] == '/') me = argv[0] + i + 1; ++i; }
    if (argc > 1 && same(argv[1], "boot")) return boot(argc, argv);
    // (needs no drive 1: the page takes it)
    if (starts(me, "download")) return download(argc, argv, 1);
    if (argc > 1 && same(argv[1], "download")) return download(argc, argv, 2);

    // the daemon is started on every desktop boot: where there is no
    // drive it simply has nothing to do
    if (!have_drive() || !load_base()) {
        if (argc > 1 && same(argv[1], "-d")) return 0;
        if (!have_drive()) ufprintf(STDERR, "persist: no drive to keep things on\n");
        else ufprintf(STDERR, "persist: this boot noted nothing to compare with\n");
        return 1;
    }

    if (starts(me, "sync") || (argc > 1 && same(argv[1], "save"))) {
        if (!lock_take(1)) { ufprintf(STDERR, "persist: another save is still running\n"); return 1; }
        survey();
        rc = save(0);
        lock_drop();
        return rc;
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
        // (into the snapshot not in use, as a save writes)
        if (!lock_take(1)) { ufprintf(STDERR, "persist: another save is still running\n"); return 1; }
        to = snap_next();
        rc = 1;
        if (w_open(to) >= 0) {
            memset(chunk, 0, BLOCK * 2);
            w_put(chunk, BLOCK * 2);
            if (w_close() >= 0 && snap_name(to) >= 0) rc = 0;
        }
        lock_drop();
        if (rc) return 1;
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
            // a save already running: this one next time
            if (!lock_take(0)) continue;
            survey();
            if (sig != last) {
                rc = save(1);
                if (rc == 0) last = sig;
            }
            freewalk();
            lock_drop();
        }
    }
    return usage();
}

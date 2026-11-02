//
// C4IX VFS: vnodes, open file descriptions, per-task fd tables.
//
// Three layers, because that is what dup2 and pipes need:
//
//   vnode   the thing itself -- console, RAM file, host file, pipe
//   file    an open description: flags plus the seek position
//   fd      an index in a per-task table, pointing at a description
//
// dup2 points two fds at ONE description, so they share a position;
// two separate opens of the same file get two descriptions and two
// positions. Spawning clones the table (bumping description refs),
// which is what makes redirection work without fork: set fd 1 up,
// spawn, put fd 1 back.
//
// RAM files are the only writable storage: this VM has no write()
// syscall, so host files stay read-only. That is not a limitation
// worth fighting -- the RAM filesystem is where a shell's `>` will
// point in X4.
//

#include "c4ix.h"

// A note on locking. Kernel TASKS (init, boot) run kernel code with
// preemption enabled, while user tasks reach this file from inside
// the trap handler, where interrupts are already off. So every entry
// point below that mutates shared state -- reference counts above
// all -- brackets itself with sched_lock. Without it a preemption
// between "--refs" and the test that follows can free a description
// another task still holds, and the corruption surfaces much later
// as a task resuming with a garbage program counter. sched_lock is
// nestable and skips the hardware entirely inside the handler, so
// this is close to free on the syscall path.

static struct sl4b_cache *vn_cache;
static struct sl4b_cache *file_cache;
static struct vnode *rootdir;      // "/" -- the RAM filesystem's root
static struct vnode *console;      // the one console vnode
static int vfs_ino;                // the last vnode number given out (stat's st_ino)

static void vfs_namecpy(char *dst, char *src) {
    int i;
    i = 0;
    while (src[i] && i < VN_NAME_MAX - 1) { dst[i] = src[i]; ++i; }
    dst[i] = 0;
}

static void vfs_namecpy2(char *dst, char *src, int len) {
    int i;
    i = 0;
    while (i < len && i < VN_NAME_MAX - 1) { dst[i] = src[i]; ++i; }
    dst[i] = 0;
}

static int vfs_nameeq(char *a, char *b) {
    int i;
    i = 0;
    while (a[i] && b[i]) {
        if (a[i] != b[i]) return 0;
        ++i;
    }
    return a[i] == b[i];
}

static int vn_grow(struct vnode *vn, int need);

// ---- /proc/uptime ----
//
// The clock, as a file: Linux's format, which is what c4m.c reads from
// its host when plain c4 runs it (c4_time_host) -- so `c4 c4m.c`, and
// innerbench -c with it, has a clock here as it has on Linux. Two
// numbers, seconds with two decimals. The first is how long the kernel
// has been up. The second is Linux's idle time summed over its CPUs;
// here it is the same clock in tenths of a second, because that is how
// c4m's default reading takes it (the integer part times 100 is
// milliseconds). `c4m -a` reads the first number instead, and oisc4
// the first with its fraction, so every reader's clock runs at the
// right speed. The bytes are made when the file is read from its start
// (vfs_read), stat'd or fetched whole (vfs_getdata), so two reads a
// second apart see two times.
static int vfs_boot_ms;            // __time() when the VFS came up

// WHOLE.CC into P; the length
static int vfs_fmt(char *p, int whole, int cents) {
    char d[16];
    int n, i;
    n = 0;
    d[n] = '0' + whole % 10; ++n; whole = whole / 10;
    while (whole) { d[n] = '0' + whole % 10; ++n; whole = whole / 10; }
    i = 0;
    while (n) { --n; p[i] = d[n]; ++i; }
    p[i] = '.';
    p[i + 1] = '0' + cents / 10;
    p[i + 2] = '0' + cents % 10;
    return i + 3;
}

static int vfs_dec(char *p, int v);
static int vfs_kv(char *p, int n, char *name, int v);

// RAM files' bytes under D, and how many: c[0], c[1] those written and not
// the host's too (persist has not saved them, or they are /tmp's)
static void vfs_ramuse(struct vnode *d, int *c) {
    struct vnode *vn;
    vn = d->child;
    while (vn) {
        if (vn->type == VN_DIR) vfs_ramuse(vn, c);
        else if (vn->type == VN_RAMFILE && vn->data && !vn->backed) { c[0] = c[0] + vn->cap; c[1] = c[1] + 1; }
        vn = vn->next;
    }
}

static void vfs_proc(struct vnode *vn) {
    int ms, n, size, r[2], c[10];
    // /proc/kmsg: the kernel log (console.c), as dmesg prints it
    if (vn->name[0] == 'k') {
        if (!vn_grow(vn, KLOG_MAX + 1)) return;
        vn->size = klog_copy(vn->data);
        return;
    }
    // /proc/meminfo: where memory is, in KB, "name value" a line (free
    // prints it): the heap, its use, what is free and the biggest block
    // of that; RAM files written and not saved, and how many; files kept
    // from the host as a cache, and how many; files' bytes left on the
    // host; and under the MMU the heap's blocks of single frames, every
    // block of frames, how many, the frames in use in them and the blocks
    // less than an eighth in use (mmu_counts), and swap
    if (vn->name[0] == 'm') {
        if (!vn_grow(vn, 512)) return;
        r[0] = 0; r[1] = 0;
        vfs_ramuse(rootdir, r);
        mmu_counts(c);
        size = *(int *)C4IX_HEAP_END - *(int *)C4IX_HEAP_BASE;
        n = vfs_kv(vn->data, 0, "heap ", size / 1024);
        if (vfs_heapinfo) {
            n = vfs_kv(vn->data, n, "used ", *(int *)C4IX_HEAP_USED / 1024);
            n = vfs_kv(vn->data, n, "free ", (size - *(int *)C4IX_HEAP_USED) / 1024);
        }
        if (c[5] >= 0) n = vfs_kv(vn->data, n, "largest ", c[5] / 1024);
        n = vfs_kv(vn->data, n, "files ", r[0] / 1024);
        n = vfs_kv(vn->data, n, "nfiles ", r[1]);
        n = vfs_kv(vn->data, n, "cached ", vfs_cachedbytes / 1024);
        n = vfs_kv(vn->data, n, "ncached ", vfs_ncached);
        n = vfs_kv(vn->data, n, "lazy ", vfs_lazybytes / 1024);
        if (mmu_on) {
            n = vfs_kv(vn->data, n, "frames ", c[0] / 1024);
            n = vfs_kv(vn->data, n, "blocks ", c[6] / 1024);
            n = vfs_kv(vn->data, n, "nblocks ", c[7]);
            n = vfs_kv(vn->data, n, "inuse ", c[8] / 1024);
            n = vfs_kv(vn->data, n, "sparse ", c[9] / 1024);
        }
        if (c[1] > 0) {
            n = vfs_kv(vn->data, n, "swap ", c[1] * 4);
            n = vfs_kv(vn->data, n, "swapfree ", c[2] * 4);
            n = vfs_kv(vn->data, n, "swapouts ", c[3]);
            n = vfs_kv(vn->data, n, "swapins ", c[4]);
        }
        vn->data[n] = 0;
        vn->size = n;
        return;
    }
    if (!vn_grow(vn, 128)) return;
    // /proc/cache: what files' bytes are where (the cache, below): files
    // still on the host and their bytes, files read in and kept and their
    // bytes, and how many times one was dropped since boot
    if (vn->name[0] == 'c') {
        n = 0;
        memcpy(vn->data, "lazy ", 5); n = 5;
        n = n + vfs_dec(vn->data + n, vfs_nlazy); vn->data[n++] = ' ';
        n = n + vfs_dec(vn->data + n, vfs_lazybytes);
        memcpy(vn->data + n, "\ncached ", 8); n = n + 8;
        n = n + vfs_dec(vn->data + n, vfs_ncached); vn->data[n++] = ' ';
        n = n + vfs_dec(vn->data + n, vfs_cachedbytes);
        memcpy(vn->data + n, "\ndropped ", 9); n = n + 9;
        n = n + vfs_dec(vn->data + n, vfs_dropped);
        vn->data[n++] = '\n';
        vn->data[n] = 0;
        vn->size = n;
        return;
    }
    ms = __time() - vfs_boot_ms;
    if (ms < 0) ms = 0;
    n = vfs_fmt(vn->data, ms / 1000, ms % 1000 / 10);
    vn->data[n] = ' '; ++n;
    n = n + vfs_fmt(vn->data + n, ms / 100, ms % 100);
    vn->data[n] = '\n'; ++n;
    vn->data[n] = 0;
    vn->size = n;
}

static void vfs_cache_init();

void vfs_init() {
    struct vnode *vn;
    con_init();
    vn_cache = sl4b_cache_create("vnode", sizeof(struct vnode), 8);
    file_cache = sl4b_cache_create("file", sizeof(struct file), 16);
    console = (struct vnode *)sl4b_alloc(vn_cache);
    console->type = VN_CONSOLE;
    console->refs = 1;
    vfs_namecpy(console->name, "console");

    rootdir = (struct vnode *)sl4b_alloc(vn_cache);
    rootdir->type = VN_DIR;
    rootdir->refs = 1;
    rootdir->parent = rootdir;        // ".." at the root is the root
    vfs_namecpy(rootdir->name, "/");
    console->ino = 1;
    rootdir->ino = 2;
    vfs_ino = 2;

    // /tmp exists from boot, for scratch files (init's demo, cc's
    // objects, the IDE's one-file build). The whole tree is writable,
    // so there is no /ram as C4KE and C4DOS have. Intermediate
    // directories are NOT created implicitly -- open() does not do
    // that in any Unix -- so this one is made here rather than
    // silently conjured on first use.
    vfs_mkdir("/tmp");
    // /proc/uptime, the clock (vfs_proc)
    vfs_boot_ms = __time();
    vfs_mkdir("/proc");
    if ((vn = vfs_ramfile("/proc/uptime"))) vn->type = VN_PROC;
    // /proc/kmsg: the kernel log, which dmesg prints (console.c)
    if ((vn = vfs_ramfile("/proc/kmsg"))) vn->type = VN_PROC;
    // /proc/cache: files on the host and in RAM (the cache, below)
    if ((vn = vfs_ramfile("/proc/cache"))) vn->type = VN_PROC;
    // /proc/meminfo: where memory is, which free prints (vfs_proc)
    if ((vn = vfs_ramfile("/proc/meminfo"))) vn->type = VN_PROC;
    vfs_cache_init();
    // /var/log/kernel: the console, logged (console.c)
    con_logopen();
    klog("vfs: / and /tmp in RAM, /proc/uptime and /proc/kmsg made when read, /var/log/kernel\n");
}

// What the boot left on the host (SYS_LAZYFILE): files, and their bytes
int vfs_nlazy, vfs_lazybytes;

// /var/log/kernel, made at boot. The kernel holds a reference, so it
// cannot be unlinked from under the console.
struct vnode *vfs_klog() {
    struct vnode *vn;
    vfs_mkdir("/var");
    vfs_mkdir("/var/log");
    if ((vn = vfs_ramfile("/var/log/kernel"))) ++vn->refs;
    return vn;
}

void vfs_logc(struct vnode *vn, int c, int max) {
    if (vn->size >= max || !vn_grow(vn, vn->size + 1)) return;
    vn->data[vn->size] = c;
    ++vn->size;
}

struct vnode *vfs_root() { return rootdir; }

int vfs_isdir(struct vnode *vn) {
    if (!vn) return 0;
    return vn->type == VN_DIR;
}

// The task's working directory, defaulting to the root for anything
// that has not set one (the boot task, or a task created before the
// VFS was up).
struct vnode *vfs_cwd() {
    struct task *t;
    if (!(t = sched_current())) return rootdir;
    if (!t->cwd) t->cwd = rootdir;
    return t->cwd;
}

// ---- path resolution ----
//
// One component at a time, which is the whole point of having
// directories: "." and ".." mean something, and a name is only ever
// matched against the entries of one directory.

static struct vnode *vfs_child(struct vnode *dir, char *name, int len) {
    struct vnode *vn;
    int i;
    if (!vfs_isdir(dir)) return 0;
    vn = dir->child;
    while (vn) {
        i = 0;
        while (i < len) {
            if (vn->name[i] != name[i]) { i = -1; break; }
            ++i;
        }
        if (i == len) { if (!vn->name[len]) return vn; }
        vn = vn->next;
    }
    return 0;
}

static struct vnode *vn_alloc(int type);

// What a directory entry names: a link's file, or the entry itself.
static struct vnode *vn_res(struct vnode *vn) {
    if (vn && vn->type == VN_LINK) return vn->target;
    return vn;
}

static struct vnode *vfs_walk2(char *path, int create, int mkdir);

// Walk PATH. When CREATE is set the final component is made as a
// file (or as a directory when MKDIR is set); intermediate
// components must already exist either way. A link at the end is
// followed to its file; vfs_walk2 is the walk that stops at the entry.
static struct vnode *vfs_walk(char *path, int create, int mkdir) {
    return vn_res(vfs_walk2(path, create, mkdir));
}

static struct vnode *vfs_walk2(char *path, int create, int mkdir) {
    struct vnode *dir;
    struct vnode *vn;
    int i, start, len;

    dir = (path[0] == '/') ? rootdir : vfs_cwd();
    i = 0;
    if (path[0] == '/') ++i;

    while (path[i]) {
        start = i;
        while (path[i]) { if (path[i] == '/') break; ++i; }
        len = i - start;
        while (path[i] == '/') ++i;              // collapse separators

        if (!len) continue;                       // trailing or doubled '/'
        if (len == 1) { if (path[start] == '.') continue; }
        if (len == 2) {
            if (path[start] == '.') {
                if (path[start + 1] == '.') { dir = dir->parent; continue; }
            }
        }

        vn = vfs_child(dir, path + start, len);
        if (!vn) {
            // only the LAST component may be created
            if (!create) return 0;
            if (path[i]) return 0;
            if (!(vn = vn_alloc(mkdir ? VN_DIR : VN_RAMFILE))) return 0;
            vfs_namecpy2(vn->name, path + start, len);
            vn->parent = dir;
            vn->next = dir->child;
            dir->child = vn;
            return vn;
        }
        if (!path[i]) return vn;                  // last component
        dir = vn;                                 // descend
    }
    return dir;                                   // "/" or "." etc.
}

static struct vnode *vn_alloc(int type) {
    struct vnode *vn;
    if (!(vn = (struct vnode *)sl4b_alloc(vn_cache))) return 0;
    vn->type = type;
    vn->refs = 0;
    vfs_ino = vfs_ino + 1;
    vn->ino = vfs_ino;
    vfs_touch(vn);
    return vn;
}

// Modification order. Not a clock -- this machine's clocks differ by
// host, and some hosts have none -- but a counter every create and
// every write takes the next value of. That orders any two changes,
// which is all a build tool needs to know which file is stale.
static int vfs_clock;

static void vn_unback(struct vnode *vn);

void vfs_touch(struct vnode *vn) {
    vfs_clock = vfs_clock + 1;
    vn->mtime = vfs_clock;
    // what it holds now is its own, not the host's
    if (vn->backed) vn_unback(vn);
    vn->sig = 0;
}

// ---- the cache: files whose bytes are the host's too ----
//
// A lazy file read in is kept (vfs_fill), but its bytes are still the
// host's: such a file is on this list, most recently used at the head,
// and when memory is short (vfs_trim) or an allocation finds none
// (vfs_malloc), files are dropped from its tail -- their bytes freed and
// the files lazy again, to be read from the host when next wanted. A
// file that changes leaves the list for good (vfs_touch). The root
// archive's files are such files where there is a drive to keep its tar
// (persist.c): they cost RAM only while in use.
enum { VFS_KEEP = 65536 };      // a lazy file this big or smaller is kept once read
int vfs_spans;                  // the host reads "NAME@OFF+LEN" (libjs; devices.js diskOpen)
static int vfs_heapinfo;        // the host says how much of the heap is in use (C4IX_HEAP_USED)
static struct vnode *vfs_chead; // most recently used
static struct vnode *vfs_ctail;
int vfs_ncached, vfs_cachedbytes, vfs_dropped;

// libjs announces both (C4IX_I_C4MJS). Native c4m reads whole files and
// says nothing of its heap: there a file is read in whole on first use,
// and dropped only when an allocation finds no memory.
static void vfs_cache_init() {
    vfs_spans = host_has(C4IX_I_C4MJS);
    vfs_heapinfo = vfs_spans;
}

static int vn_cached(struct vnode *vn) {
    return vn->cprev || vfs_chead == vn;
}

static void cache_out(struct vnode *vn) {
    if (!vn_cached(vn)) return;
    if (vn->cprev) vn->cprev->cnext = vn->cnext; else vfs_chead = vn->cnext;
    if (vn->cnext) vn->cnext->cprev = vn->cprev; else vfs_ctail = vn->cprev;
    vn->cnext = 0;
    vn->cprev = 0;
    --vfs_ncached;
    vfs_cachedbytes = vfs_cachedbytes - vn->cap;
}

static void cache_in(struct vnode *vn) {
    vn->cprev = 0;
    vn->cnext = vfs_chead;
    if (vfs_chead) vfs_chead->cprev = vn; else vfs_ctail = vn;
    vfs_chead = vn;
    ++vfs_ncached;
    vfs_cachedbytes = vfs_cachedbytes + vn->cap;
}

// it was used: to the head
static void cache_use(struct vnode *vn) {
    if (!vn_cached(vn) || vfs_chead == vn) return;
    cache_out(vn);
    cache_in(vn);
}

static void vn_unback(struct vnode *vn) {
    cache_out(vn);
    vn->backed = 0;
}

// its bytes freed: lazy again
static void vn_drop(struct vnode *vn) {
    cache_out(vn);
    free(vn->data);
    vn->data = 0;
    vn->cap = 0;
    vn->lazy = 1;
    ++vfs_nlazy;
    vfs_lazybytes = vfs_lazybytes + vn->size;
    ++vfs_dropped;
}

// no longer lazy: its bytes are in RAM, or it was emptied
static void vn_unlazy(struct vnode *vn) {
    if (!vn->lazy) return;
    vn->lazy = 0;
    --vfs_nlazy;
    vfs_lazybytes = vfs_lazybytes - vn->size;
}

static int vfs_short() {
    int used, size;
    if (!vfs_heapinfo) return 0;
    used = *(int *)C4IX_HEAP_USED;
    size = *(int *)C4IX_HEAP_END - *(int *)C4IX_HEAP_BASE;
    return used > size / 8 * 7;
}

// May the cache let VN go? Not while it is open for writing, or held.
static int vn_droppable(struct vnode *vn) {
    return !vn->held && !vn->writers;
}

int vfs_reclaim(int want) {
    struct vnode *vn, *p;
    int freed;
    freed = 0;
    if (!vfs_ctail) return 0;
    sched_lock();
    vn = vfs_ctail;
    while (vn && freed < want) {
        p = vn->cprev;
        if (vn_droppable(vn)) { freed = freed + vn->cap; vn_drop(vn); }
        vn = p;
    }
    sched_unlock();
    return freed;
}

void vfs_trim() {
    struct vnode *vn, *p;
    if (!vfs_ctail || !vfs_short()) return;
    sched_lock();
    vn = vfs_ctail;
    while (vn && vfs_short()) {
        p = vn->cprev;
        if (vn_droppable(vn)) vn_drop(vn);
        vn = p;
    }
    sched_unlock();
}

char *vfs_malloc(int n) {
    char *p;
    vfs_trim();
    while (!(p = (char *)malloc(n))) {
        if (!vfs_reclaim(n)) return mmu_kalloc(n);
    }
    return p;
}

// Growable storage for RAM files and pipes. c4m's realloc opcode is
// the known-broken one, so this is malloc + copy + free by hand. A
// file grown is about to change: it leaves the cache.
static int vn_grow(struct vnode *vn, int need) {
    char *nd;
    int cap;

    if (vn->cap >= need) return 1;
    if (vn->backed) vn_unback(vn);
    cap = vn->cap ? vn->cap : 64;
    while (cap < need) cap = cap * 2;
    if (!(nd = vfs_malloc(cap))) return 0;
    memset(nd, 0, cap);
    if (vn->size) memcpy(nd, vn->data, vn->size);
    if (vn->data) free(vn->data);
    vn->data = nd;
    vn->cap = cap;
    return 1;
}

static void vn_release(struct vnode *vn) {
    --vn->refs;
    if (vn->refs > 0) return;
    if (vn->type == VN_HOSTFILE) { close(vn->host); }
    // RAM files and directories outlive their descriptors: they ARE
    // the filesystem.
    if (vn->type == VN_RAMFILE) return;
    if (vn->type == VN_PROC) return;
    if (vn->type == VN_DIR) return;
    if (vn->type == VN_CONSOLE) return;
    if (vn->data) free(vn->data);
    sl4b_free(vn_cache, (char *)vn);
}

struct vnode *vfs_lookup(char *path) {
    return vfs_walk(path, 0, 0);
}

struct vnode *vfs_ramfile(char *path) {
    struct vnode *vn;
    sched_lock();
    vn = vfs_walk(path, 1, 0);
    sched_unlock();
    return vn;
}

// A RAM file whose bytes stay on the host until something needs them.
// vfsload makes these at boot instead of copying every file in its
// manifest: on a host that fetches files over a network (libjs in a
// browser) that is the difference between a boot that downloads the
// whole disk and one that downloads what it runs. The size is known
// now, so stat and ls are right before anything is read. HOST may be a
// span of a host file, "NAME@OFF+LEN" (vfs_spans): persist.c makes the
// root archive's files so, each its bytes in the tar kept on drive 1.
// It is a change, as writing the file would be: it takes the next
// modification stamp, so files made in a tar's order are ordered as
// unpacking them was.
int vfs_lazyfile(char *path, char *host, int size) {
    struct vnode *vn;
    char *h;
    int n;
    sched_lock();
    if (!(vn = vfs_walk(path, 1, 0)) || vn->type != VN_RAMFILE) { sched_unlock(); return -1; }
    n = 0; while (host[n]) ++n;
    if (!(h = vfs_malloc(n + 1))) { sched_unlock(); return -1; }
    memcpy(h, host, n + 1);
    if (vn->hostname) free(vn->hostname);
    vn->hostname = h;
    vfs_touch(vn);
    vn_unlazy(vn);
    if (vn->data) { free(vn->data); vn->data = 0; vn->cap = 0; }
    vn->size = size;
    vn->lazy = 1;
    vn->backed = 1;
    ++vfs_nlazy;
    vfs_lazybytes = vfs_lazybytes + size;
    sched_unlock();
    return 0;
}

// PATH, saved (persist.c), is the host's again at HOST, its span of the
// snapshot just written: unless it has changed since STAMP, whose bytes
// are newer than the saved ones. It did not change, so its stamp stays
// (persist -d would see a change at every save otherwise). Read from
// there when wanted, as a file of the kept system is: a big one's bytes
// freed now, a small one's kept in the cache, to go when memory is short;
// while it is open to write or held, the cache keeps it (vn_droppable).
int vfs_relazy(char *path, char *host, int stamp) {
    struct vnode *vn;
    char *h;
    int n;
    sched_lock();
    if (!(vn = vfs_walk(path, 0, 0)) || vn->type != VN_RAMFILE || vn->mtime != stamp) { sched_unlock(); return -1; }
    n = 0; while (host[n]) ++n;
    // (taking the memory may drop it from the cache: lazy, as below)
    if (!(h = vfs_malloc(n + 1))) { sched_unlock(); return -1; }
    memcpy(h, host, n + 1);
    if (vn->hostname) free(vn->hostname);
    vn->hostname = h;
    vn->backed = 1;
    if (!vn->lazy) {
        if (vn->size > VFS_KEEP && vn_droppable(vn)) {
            cache_out(vn);
            free(vn->data);
            vn->data = 0;
            vn->cap = 0;
            vn->lazy = 1;
            ++vfs_nlazy;
            vfs_lazybytes = vfs_lazybytes + vn->size;
        } else if (!vn_cached(vn)) cache_in(vn);
    }
    sched_unlock();
    return 0;
}

// P from N gets "NAME V\n" (V not below 0); where it ends
static int vfs_kv(char *p, int n, char *name, int v) {
    while (*name) p[n++] = *name++;
    n = n + vfs_dec(p + n, v < 0 ? 0 : v);
    p[n++] = '\n';
    return n;
}

// P gets V in decimal; its length
static int vfs_dec(char *p, int v) {
    char d[12];
    int n, i;
    n = 0;
    d[n++] = '0' + v % 10; v = v / 10;
    while (v > 0) { d[n++] = '0' + v % 10; v = v / 10; }
    i = 0;
    while (n) p[i++] = d[--n];
    p[i] = 0;
    return i;
}

// Is this host name a span, NAME@OFF+LEN? Where its '@' is, or -1.
static int vfs_span_at(char *h) {
    int n, i, plus;
    n = 0; while (h[n]) ++n;
    i = n - 1; plus = -1;
    while (i > 0 && ((h[i] >= '0' && h[i] <= '9') || (h[i] == '+' && plus < 0))) {
        if (h[i] == '+') plus = i;
        --i;
    }
    if (i > 0 && h[i] == '@' && plus > i + 1 && plus < n - 1) return i;
    return -1;
}

// LEN bytes of VN's host bytes from POS into BUF, through a span; the
// count, or -1. A span's own offset is added to POS.
static int vfs_span_read(struct vnode *vn, char *buf, int pos, int len) {
    char name[PATH_MAX + 32];
    int at, off, i, k, fd, got, n;
    at = vfs_span_at(vn->hostname);
    off = 0;
    if (at >= 0) {
        i = at + 1;
        while (vn->hostname[i] != '+') { off = off * 10 + vn->hostname[i] - '0'; ++i; }
    } else { at = 0; while (vn->hostname[at]) ++at; }
    if (at >= PATH_MAX) return -1;
    memcpy(name, vn->hostname, at);
    k = at;
    name[k++] = '@';
    k = k + vfs_dec(name + k, off + pos);
    name[k++] = '+';
    vfs_dec(name + k, len);
    if ((fd = open(name, 0)) < 0) return -1;
    got = 0;
    while (got < len && (n = read(fd, buf + got, len - got)) > 0) got = got + n;
    close(fd);
    return got;
}

// Read a lazy file's bytes in from the host, and keep them: it is an
// ordinary RAM file from now on, and a cached one (above) while it does
// not change. A host file that has gone leaves it empty. With no memory
// for it, even once the cache has given up what it can, it stays lazy,
// and 0.
int vfs_fill(struct vnode *vn) {
    int fd, n, size, got;
    char *nd;
    if (!vn || !vn->lazy) return 1;
    size = vn->size;
    if (!(nd = vfs_malloc(size + 1))) return 0;
    if ((fd = open(vn->hostname, 0)) < 0) {
        free(nd);
        vn_unlazy(vn);
        vn->size = 0;
        return 0;
    }
    got = 0;
    while (got < size && (n = read(fd, nd + got, size - got)) > 0) got = got + n;
    close(fd);
    nd[got] = 0;
    vn_unlazy(vn);
    vn->data = nd;
    vn->cap = size + 1;
    vn->size = got;
    if (vn->backed) cache_in(vn);
    return 1;
}

// LEN bytes of a RAM file from POS, into BUF: from RAM, or from the host
// where it is still there and the host reads spans. Fewer at the end;
// -1 when they cannot be had.
int vfs_pread(struct vnode *vn, char *buf, int pos, int len) {
    int n;
    if (pos < 0 || len < 0) return -1;
    sched_lock();
    n = vn->size - pos;
    if (n > len) n = len;
    if (n <= 0) { sched_unlock(); return 0; }
    if (vn->lazy && vfs_spans) n = vfs_span_read(vn, buf, pos, n);
    else if (vn->lazy && !vfs_fill(vn)) n = -1;
    else {
        if (n > vn->size - pos) n = vn->size - pos;   // a host file that went
        if (n < 0) n = 0;
        memcpy(buf, vn->data + pos, n);
        cache_use(vn);
    }
    sched_unlock();
    return n;
}

// What a path is, for a file browser: out[0] is 1 for a directory, 2 for
// a RAM file, 3 for anything else (the console, a pipe); out[1] is the
// size in bytes of a RAM file. -1 when there is no such entry.
// Is this file a program? A loaded file by its C4R signature; a lazy
// one by the host name behind it, so asking never fetches the file (a
// file browser asks for a whole folder at a time) -- except a span of a
// host file (the root archive's, in the tar kept on drive 1), which is
// in the host's memory already: its first three bytes, once.
static int vn_is_program(struct vnode *vn) {
    char s[4];
    int n;
    if (vn->type != VN_RAMFILE) return 0;
    if (vn->lazy && vfs_spans && vfs_span_at(vn->hostname) >= 0) {
        if (!vn->sig) vn->sig = vn->size >= 13 && vfs_span_read(vn, s, 0, 3) == 3 && s[0] == 'C' && s[1] == '4' && s[2] == 'R' ? 2 : 1;
        return vn->sig == 2;
    }
    if (vn->lazy) {
        n = 0;
        while (vn->hostname[n]) ++n;
        return n > 4 && vn->hostname[n - 4] == '.' && vn->hostname[n - 3] == 'c' &&
               vn->hostname[n - 2] == '4' && vn->hostname[n - 1] == 'r';
    }
    return vn->size >= 13 && vn->data[0] == 'C' && vn->data[1] == '4' && vn->data[2] == 'R';
}

// The first N words of the ST_* record (c4ix.h): out[0] 1 dir, 2 file,
// 3 other; out[1] bytes; out[2] 1 for a program; out[3] mtime; then the
// number, the names, a mode and a device. N is 4 for the callers from
// before there were more. The modes are made up -- there are no owners
// or permissions -- but say what ls and cp need: directories and
// programs are 0755, files 0644, the console and pipes 0666.
void vn_stat(struct vnode *vn, int *out, int n) {
    int st[ST_WORDS];
    int i;
    if (vn->type == VN_PROC) {
        // a file to anything that asks, read-only, as long as it is now
        vfs_proc(vn);
        st[ST_TYPE] = 2; st[ST_SIZE] = vn->size; st[ST_PROG] = 0; st[ST_MTIME] = vn->mtime;
        st[ST_INO] = vn->ino; st[ST_NLINK] = 1; st[ST_MODE] = 33060; st[ST_DEV] = 1;   // S_IFREG|0444
        if (n <= 0) n = 4;
        if (n > ST_WORDS) n = ST_WORDS;
        i = 0;
        while (i < n) { out[i] = st[i]; ++i; }
        return;
    }
    st[ST_TYPE] = vn->type == VN_DIR ? 1 : (vn->type == VN_RAMFILE ? 2 : 3);
    st[ST_SIZE] = vn->type == VN_RAMFILE ? vn->size : 0;
    st[ST_PROG] = vn_is_program(vn);
    st[ST_MTIME] = vn->mtime;
    st[ST_INO] = vn->ino;
    st[ST_NLINK] = 1 + vn->nlinks;
    st[ST_MODE] = vn->type == VN_DIR ? 16877 : vn->type == VN_RAMFILE ? (st[ST_PROG] ? 33261 : 33188)
                : vn->type == VN_PIPE ? 4534 : 8630;   // S_IFDIR|0755, S_IFREG|0755 or 0644, S_IFIFO|0666, S_IFCHR|0666
    st[ST_DEV] = (vn->type == VN_RAMFILE || vn->type == VN_DIR) ? 1 : vn->type == VN_HOSTFILE ? 2 : 0;
    if (n <= 0) n = 4;
    if (n > ST_WORDS) n = ST_WORDS;
    i = 0;
    while (i < n) { out[i] = st[i]; ++i; }
}

int vfs_stat(char *path, int *out, int n) {
    struct vnode *vn;
    sched_lock();
    if (!(vn = vfs_walk(path, 0, 0))) { sched_unlock(); return -1; }
    vn_stat(vn, out, n);
    sched_unlock();
    return 0;
}

// ---- links and truncation ----

// A second name, NEWPATH, for the RAM file PATH: a VN_LINK entry in
// NEWPATH's directory. Only files: a directory with two parents would
// make ".." mean two things. NEWPATH must not exist.
int vfs_link(char *path, char *newpath) {
    struct vnode *vn, *l;
    sched_lock();
    if (!(vn = vfs_walk(path, 0, 0)) || vn->type != VN_RAMFILE || vfs_walk2(newpath, 0, 0)) {
        sched_unlock();
        return -1;
    }
    if (!(l = vfs_walk2(newpath, 1, 0))) { sched_unlock(); return -1; }   // its directory is not there
    l->type = VN_LINK;
    l->target = vn;
    l->lnext = vn->links;
    vn->links = l;
    ++vn->nlinks;
    sched_unlock();
    return 0;
}

// LEN bytes: what was past it goes, and a file grown gets zeros
int vfs_truncate(struct vnode *vn, int len) {
    if (!vn || vn->type != VN_RAMFILE || len < 0) return -1;
    sched_lock();
    // emptied: the host's bytes are not wanted
    if (vn->lazy && !len) { vn_unlazy(vn); vn->size = 0; }
    if (vn->lazy && !vfs_fill(vn)) { sched_unlock(); return -1; }
    if (len > vn->size) {
        if (!vn_grow(vn, len)) { sched_unlock(); return -1; }
        memset(vn->data + vn->size, 0, len - vn->size);
    }
    vn->size = len;
    vfs_touch(vn);
    sched_unlock();
    return 0;
}

// Take VN out of its directory's list
static void vn_detach(struct vnode *vn) {
    struct vnode *p, *q;
    p = vn->parent;
    if (p->child == vn) p->child = vn->next;
    else {
        q = p->child;
        while (q && q->next != vn) q = q->next;
        if (q) q->next = vn->next;
    }
    vn->next = 0;
}

// ---- C4KE's kernel RAM filesystem, over this one ----
//
// C4KE's compilers (c4cc, c4rlink, c4sp's c4lc) write what they build
// with OP_VFS_PUT and read with OP_VFS_GET, because the VM has no write
// opcode. C4KE's table is flat; here a name is a path, so a bare name
// lands in the task's working directory, which is where C4KE's flat
// names meant it. GET hands back the kernel's own bytes, NUL-terminated
// after the end, as C4KE's does: one address space, so the task reads
// them in place.

int vfs_put(char *path, char *buf, int len) {
    struct vnode *vn;
    if (len < 0) return -1;
    sched_lock();
    vn = vfs_walk(path, 1, 0);
    if (!vn || vn->type != VN_RAMFILE) { sched_unlock(); return -1; }
    vn_unlazy(vn);
    vn->size = 0;
    if (!vn_grow(vn, len + 1)) { sched_unlock(); return -1; }
    memcpy(vn->data, buf, len);
    vn->data[len] = 0;
    vn->size = len;
    vfs_touch(vn);
    sched_unlock();
    return 0;
}

char *vfs_getdata(char *path, int *plen, struct vnode **hold) {
    struct vnode *vn;
    sched_lock();
    vn = vfs_walk(path, 0, 0);
    if (vn && vn->type == VN_PROC) vfs_proc(vn);
    else if (!vn || vn->type != VN_RAMFILE) { sched_unlock(); return 0; }
    if (vn->lazy && !vfs_fill(vn)) { sched_unlock(); return 0; }
    if (!vn_grow(vn, vn->size + 1)) { sched_unlock(); return 0; }
    vn->data[vn->size] = 0;
    if (plen) *plen = vn->size;
    // the caller reads them in place: not dropped while it does, or ever
    if (hold) { ++vn->held; *hold = vn; cache_use(vn); }
    else if (vn->backed) vn_unback(vn);
    sched_unlock();
    return vn->data;
}

void vfs_unhold(struct vnode *vn) {
    if (!vn) return;
    sched_lock();
    --vn->held;
    sched_unlock();
}

// The files (not folders) of the working directory, for COUNT and NAME.
static int vn_isfile(struct vnode *vn) {
    return vn->type == VN_RAMFILE || vn->type == VN_PROC;
}

static struct vnode *vfs_nth_file(int i) {
    struct vnode *vn;
    vn = vfs_cwd()->child;
    while (vn) {
        if (vn_isfile(vn_res(vn))) { if (!i) return vn; --i; }
        vn = vn->next;
    }
    return 0;
}

int vfs_nfiles() {
    struct vnode *vn;
    int n;
    n = 0;
    vn = vfs_cwd()->child;
    while (vn) { if (vn_isfile(vn_res(vn))) ++n; vn = vn->next; }
    return n;
}

char *vfs_fname(int i) {
    struct vnode *vn;
    if (i < 0 || !(vn = vfs_nth_file(i))) return 0;
    return vn->name;
}

// Take an entry out of its directory. A directory must be empty, and a
// file must not be open. A removed file's storage is freed; a removed
// directory's vnode is not, because some task may still have it as its
// working directory -- it lives on, detached, and costs one small struct.
//
// A name of a file that has others (SYS_LINK) goes, and the file stays.
// A link entry is simply dropped. The file's own entry, its first name,
// is the vnode itself, so the file moves into its first link's place and
// takes that link's name; open descriptors, which point at the vnode,
// never notice.
int vfs_unlink(char *path) {
    struct vnode *vn, *l;
    sched_lock();
    vn = vfs_walk2(path, 0, 0);
    if (!vn || vn == rootdir || !vn->parent ||
        (vn->type == VN_DIR && vn->child) ||
        (vn->type != VN_DIR && vn->type != VN_RAMFILE && vn->type != VN_LINK)) { sched_unlock(); return -1; }
    if (vn->type == VN_LINK) {
        vn_detach(vn);
        l = vn->target;
        if (l->links == vn) l->links = vn->lnext;
        else {
            l = l->links;
            while (l->lnext != vn) l = l->lnext;
            l->lnext = vn->lnext;
        }
        --vn->target->nlinks;
        sl4b_free(vn_cache, (char *)vn);
        sched_unlock();
        return 0;
    }
    if (vn->nlinks) {
        l = vn->links;
        vn->links = l->lnext;
        --vn->nlinks;
        vn_detach(vn);
        vn_detach(l);
        vfs_namecpy(vn->name, l->name);
        vn->parent = l->parent;
        vn->next = vn->parent->child;
        vn->parent->child = vn;
        sl4b_free(vn_cache, (char *)l);
        sched_unlock();
        return 0;
    }
    if (vn->refs > 0) { sched_unlock(); return -1; }
    vn_detach(vn);
    if (vn->type == VN_RAMFILE) {
        cache_out(vn);
        vn_unlazy(vn);
        if (vn->data) free(vn->data);
        if (vn->hostname) free(vn->hostname);
        sl4b_free(vn_cache, (char *)vn);
    }
    sched_unlock();
    return 0;
}

// A new name in the same directory. NEWNAME is one component: no '/',
// not "." or "..", and not a name the directory already has.
int vfs_rename(char *path, char *newname) {
    struct vnode *vn;
    int n;
    n = 0;
    while (newname[n]) { if (newname[n] == '/') return -1; ++n; }
    if (!n || n >= VN_NAME_MAX) return -1;
    if (newname[0] == '.' && (n == 1 || (n == 2 && newname[1] == '.'))) return -1;
    sched_lock();
    vn = vfs_walk2(path, 0, 0);   // a link is renamed, not its file
    if (!vn || vn == rootdir || !vn->parent || vn->type == VN_PROC || vfs_child(vn->parent, newname, n)) { sched_unlock(); return -1; }
    vfs_namecpy(vn->name, newname);
    sched_unlock();
    return 0;
}

struct vnode *vfs_mkdir(char *path) {
    struct vnode *vn;
    sched_lock();
    if ((vn = vfs_walk(path, 0, 0))) { sched_unlock(); return 0; }  // exists
    vn = vfs_walk(path, 1, 1);
    sched_unlock();
    return vn;
}

int vfs_chdir(char *path) {
    struct vnode *vn;
    struct task *t;
    if (!(vn = vfs_lookup(path))) return -1;
    if (!vfs_isdir(vn)) return -1;
    if (!(t = sched_current())) return -1;
    t->cwd = vn;
    return 0;
}

// Entry INDEX of DIR. Returns 1 if there was one, 0 past the end.
int vfs_direntry(struct vnode *dir, int index, char *name, int *isdir) {
    struct vnode *vn;
    int i;
    if (!vfs_isdir(dir)) return 0;
    sched_lock();
    vn = dir->child;
    i = 0;
    while (vn) {
        if (i == index) {
            vfs_namecpy(name, vn->name);
            *isdir = vfs_isdir(vn_res(vn));
            sched_unlock();
            return 1;
        }
        ++i;
        vn = vn->next;
    }
    sched_unlock();
    return 0;
}

struct vnode *vfs_pipe() {
    return vn_alloc(VN_PIPE);
}

// Wrap an already-open host descriptor. Read-only by nature: the VM
// has no write() syscall to offer.
struct vnode *vn_hostfile(int host) {
    struct vnode *vn;
    if (!(vn = vn_alloc(VN_HOSTFILE))) return 0;
    vn->host = host;
    vfs_namecpy(vn->name, "host");
    return vn;
}

// ---- reading and writing through a description ----

// A pipe read blocks when the buffer is drained and a writer is
// still around; once the last writer closes, the same emptiness
// means end of file instead.
//
// The console answers the same question, which is what lets a task
// waiting for a keystroke park in TS_BLOCKED like any other blocked
// reader instead of stopping the machine inside a host read.
int vfs_readable(struct vnode *vn, int pos) {
    if (vn->type == VN_CONSOLE) return con_poll();
    if (vn->type != VN_PIPE) return 1;
    if (vn->size > vn->rpos) return 1;
    return vn->writers == 0;
}

int vfs_read(struct file *f, char *buf, int len) {
    struct vnode *vn;
    int n, i;

    sched_lock();
    vn = f->vn;
    if (vn->type == VN_CONSOLE) { sched_unlock(); return con_read(buf, len); }
    if (vn->type == VN_HOSTFILE) { sched_unlock(); return read(vn->host, buf, len); }

    if (vn->type == VN_PIPE) {
        n = vn->size - vn->rpos;
        if (n <= 0) { sched_unlock(); return 0; }   // caller checked readable
        if (n > len) n = len;
        memcpy(buf, vn->data + vn->rpos, n);
        vn->rpos = vn->rpos + n;
        if (vn->rpos == vn->size) { vn->rpos = 0; vn->size = 0; }
        sched_unlock();
        return n;
    }

    // RAM file: read from the description's own position. /proc/uptime
    // is one too, made again whenever it is read from the start.
    if (vn->type == VN_PROC && !f->pos) vfs_proc(vn);
    // Still on the host: a small file is read in and kept, while there
    // is room; a big one -- a game's data -- is read where it is, a span
    // at a time, and never takes the RAM
    if (vn->lazy && (!vfs_spans || (vn->size <= VFS_KEEP && !vfs_short()))) {
        if (!vfs_fill(vn) && !vfs_spans) { sched_unlock(); return -1; }
    }
    if (vn->lazy) {
        n = vfs_pread(vn, buf, f->pos, len);
        if (n > 0) f->pos = f->pos + n;
        sched_unlock();
        return n;
    }
    n = vn->size - f->pos;
    if (n <= 0) { sched_unlock(); return 0; }
    if (n > len) n = len;
    memcpy(buf, vn->data + f->pos, n);
    f->pos = f->pos + n;
    cache_use(vn);
    sched_unlock();
    return n;
}

int vfs_write(struct file *f, char *buf, int len) {
    struct vnode *vn;
    int i, end;

    sched_lock();
    vn = f->vn;
    if (vn->type == VN_CONSOLE) {
        con_write_task(buf, len);
        sched_unlock();
        return len;
    }
    if (vn->type == VN_HOSTFILE) { sched_unlock(); return -1; }  // no write()
    if (vn->type == VN_DIR || vn->type == VN_PROC) { sched_unlock(); return -1; }

    if (vn->type == VN_PIPE) {
        // Compact first: a drained pipe reuses its buffer instead of
        // growing forever under a steady producer.
        if (vn->rpos && vn->rpos == vn->size) { vn->rpos = 0; vn->size = 0; }
        if (!vn_grow(vn, vn->size + len)) { sched_unlock(); return -1; }
        memcpy(vn->data + vn->size, buf, len);
        vn->size = vn->size + len;
        sched_unlock();
        return len;
    }

    // RAM file: write at the description's position, extending as
    // needed, or at the end for O_APPEND.
    if (vn->lazy && !vfs_fill(vn)) { sched_unlock(); return -1; }
    if (vn->backed) vn_unback(vn);
    if (f->flags & C4IX_O_APPEND) f->pos = vn->size;
    end = f->pos + len;
    if (!vn_grow(vn, end)) { sched_unlock(); return -1; }
    // past the end (lseek): the gap reads as zeros, whatever a truncate left in the buffer
    if (f->pos > vn->size) memset(vn->data + vn->size, 0, f->pos - vn->size);
    memcpy(vn->data + f->pos, buf, len);
    vfs_touch(vn);
    f->pos = end;
    if (end > vn->size) vn->size = end;
    sched_unlock();
    return len;
}

// Bytes currently stored in a RAM file, or -1 if there is no such file.
int vfs_size(char *name) {
    struct vnode *vn;
    if (!(vn = vfs_lookup(name))) return -1;
    return vn->size;
}

// Kernel-side convenience: print a RAM file's contents to the
// console, used by init to show that redirection captured something.
void vfs_dump(char *name) {
    struct vnode *vn;
    int i;
    if (!(vn = vfs_lookup(name))) { kprintf("(no such ram file: %s)\n", name); return; }
    if (vn->lazy) vfs_fill(vn);
    i = 0;
    while (i < vn->size) { kputc(vn->data[i]); ++i; }
}

// ---- open file descriptions ----

static struct file *file_alloc(struct vnode *vn, int flags) {
    struct file *f;
    if (!(f = (struct file *)sl4b_alloc(file_cache))) return 0;
    sched_lock();
    f->vn = vn;
    f->flags = flags;
    f->pos = 0;
    f->refs = 1;
    ++vn->refs;
    // a pipe's readers see end of file once there are none; a cached
    // file is not dropped while there are any
    if ((flags & 3) != C4IX_O_RDONLY) ++vn->writers;
    sched_unlock();
    return f;
}

static void file_unref(struct file *f) {
    sched_lock();
    --f->refs;
    if (f->refs > 0) { sched_unlock(); return; }
    if ((f->flags & 3) != C4IX_O_RDONLY) --f->vn->writers;
    vn_release(f->vn);
    sl4b_free(file_cache, (char *)f);
    sched_unlock();
}

// ---- per-task descriptor tables ----

struct file *fd_get(struct task *t, int fd) {
    if (fd < 0 || fd >= FD_MAX) return 0;
    return (struct file *)t->fds[fd];
}

int fd_install(struct task *t, struct file *f) {
    int fd;
    sched_lock();
    fd = 0;
    while (fd < FD_MAX) {
        if (!t->fds[fd]) { t->fds[fd] = (int)f; sched_unlock(); return fd; }
        ++fd;
    }
    sched_unlock();
    return -1;
}

int fd_open_vnode(struct task *t, struct vnode *vn, int flags) {
    struct file *f;
    int fd;
    if (!(f = file_alloc(vn, flags))) return -1;
    if ((fd = fd_install(t, f)) < 0) { file_unref(f); return -1; }
    return fd;
}

int fd_close(struct task *t, int fd) {
    struct file *f;
    sched_lock();
    if (!(f = fd_get(t, fd))) { sched_unlock(); return -1; }
    t->fds[fd] = 0;
    t->fdcloexec[fd] = 0;
    file_unref(f);
    sched_unlock();
    return 0;
}

// Point newfd at oldfd's description: same flags, same position.
int fd_dup2(struct task *t, int oldfd, int newfd) {
    struct file *f;
    sched_lock();
    if (!(f = fd_get(t, oldfd))) { sched_unlock(); return -1; }
    if (newfd < 0 || newfd >= FD_MAX) { sched_unlock(); return -1; }
    if (oldfd == newfd) { sched_unlock(); return newfd; }
    if (t->fds[newfd]) fd_close(t, newfd);
    ++f->refs;
    t->fds[newfd] = (int)f;
    t->fdcloexec[newfd] = 0;          // a dup is inherited unless asked otherwise
    sched_unlock();
    return newfd;
}

void fd_init_console(struct task *t) {
    fd_open_vnode(t, console, C4IX_O_RDONLY);   // 0
    fd_open_vnode(t, console, C4IX_O_WRONLY);   // 1
    fd_open_vnode(t, console, C4IX_O_WRONLY);   // 2
}

// Inherit the parent's table -- descriptions are shared, not copied,
// so a redirected fd 1 stays redirected in the child. A descriptor the
// parent marked close-on-spawn (SYS_CLOEXEC) is left out: the desktop
// keeps its end of each terminal's pipes that way, so no shell holds the
// write end of its own stdin (it would never see end of file) or any
// other terminal's pipes.
void fd_clone(struct task *dst, struct task *src) {
    struct file *f;
    int fd;
    sched_lock();
    fd = 0;
    while (fd < FD_MAX) {
        if ((f = fd_get(src, fd)) && !src->fdcloexec[fd]) {
            ++f->refs;
            dst->fds[fd] = (int)f;
        }
        ++fd;
    }
    sched_unlock();
}

// vfork: the child has every descriptor the parent has, close-on-exec
// ones too, and with the same marks; its execve closes them.
void fd_clone_all(struct task *dst, struct task *src) {
    struct file *f;
    int fd;
    sched_lock();
    fd = 0;
    while (fd < FD_MAX) {
        if ((f = fd_get(src, fd))) {
            ++f->refs;
            dst->fds[fd] = (int)f;
            dst->fdcloexec[fd] = src->fdcloexec[fd];
        }
        ++fd;
    }
    sched_unlock();
}

void fd_close_on_exec(struct task *t) {
    int fd;
    sched_lock();
    fd = 0;
    while (fd < FD_MAX) {
        if (t->fds[fd] && t->fdcloexec[fd]) fd_close(t, fd);
        ++fd;
    }
    sched_unlock();
}

void fd_closeall(struct task *t) {
    int fd;
    sched_lock();
    fd = 0;
    while (fd < FD_MAX) {
        if (t->fds[fd]) fd_close(t, fd);
        ++fd;
    }
    sched_unlock();
}

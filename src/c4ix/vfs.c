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

static struct sl4b_cache *vn_cache;
static struct sl4b_cache *file_cache;
static struct vnode *ramfs;        // the named RAM files
static struct vnode *console;      // the one console vnode

static void vfs_namecpy(char *dst, char *src) {
    int i;
    i = 0;
    while (src[i] && i < VN_NAME_MAX - 1) { dst[i] = src[i]; ++i; }
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

void vfs_init() {
    vn_cache = sl4b_cache_create("vnode", sizeof(struct vnode), 8);
    file_cache = sl4b_cache_create("file", sizeof(struct file), 16);
    console = (struct vnode *)sl4b_alloc(vn_cache);
    console->type = VN_CONSOLE;
    console->refs = 1;
    vfs_namecpy(console->name, "console");
}

static struct vnode *vn_alloc(int type) {
    struct vnode *vn;
    if (!(vn = (struct vnode *)sl4b_alloc(vn_cache))) return 0;
    vn->type = type;
    vn->refs = 0;
    return vn;
}

// Growable storage for RAM files and pipes. c4m's realloc opcode is
// the known-broken one, so this is malloc + copy + free by hand.
static int vn_grow(struct vnode *vn, int need) {
    char *nd;
    int cap;

    if (vn->cap >= need) return 1;
    cap = vn->cap ? vn->cap : 64;
    while (cap < need) cap = cap * 2;
    if (!(nd = (char *)malloc(cap))) return 0;
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
    // RAM files outlive their descriptors: they are the filesystem.
    if (vn->type == VN_RAMFILE) return;
    if (vn->type == VN_CONSOLE) return;
    if (vn->data) free(vn->data);
    sl4b_free(vn_cache, (char *)vn);
}

struct vnode *vfs_lookup(char *name) {
    struct vnode *vn;
    vn = ramfs;
    while (vn) {
        if (vfs_nameeq(vn->name, name)) return vn;
        vn = vn->next;
    }
    return 0;
}

struct vnode *vfs_ramfile(char *name) {
    struct vnode *vn;
    if ((vn = vfs_lookup(name))) return vn;
    if (!(vn = vn_alloc(VN_RAMFILE))) return 0;
    vfs_namecpy(vn->name, name);
    vn->next = ramfs;
    ramfs = vn;
    return vn;
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
int vfs_readable(struct vnode *vn, int pos) {
    if (vn->type != VN_PIPE) return 1;
    if (vn->size > vn->rpos) return 1;
    return vn->writers == 0;
}

int vfs_read(struct file *f, char *buf, int len) {
    struct vnode *vn;
    int n, i;

    vn = f->vn;
    if (vn->type == VN_CONSOLE) return read(0, buf, len);
    if (vn->type == VN_HOSTFILE) return read(vn->host, buf, len);

    if (vn->type == VN_PIPE) {
        n = vn->size - vn->rpos;
        if (n <= 0) return 0;                 // caller checked readable
        if (n > len) n = len;
        i = 0;
        while (i < n) { buf[i] = vn->data[vn->rpos + i]; ++i; }
        vn->rpos = vn->rpos + n;
        if (vn->rpos == vn->size) { vn->rpos = 0; vn->size = 0; }
        return n;
    }

    // RAM file: read from the description's own position
    n = vn->size - f->pos;
    if (n <= 0) return 0;
    if (n > len) n = len;
    i = 0;
    while (i < n) { buf[i] = vn->data[f->pos + i]; ++i; }
    f->pos = f->pos + n;
    return n;
}

int vfs_write(struct file *f, char *buf, int len) {
    struct vnode *vn;
    int i, end;

    vn = f->vn;
    if (vn->type == VN_CONSOLE) {
        i = 0;
        while (i < len) { kputc(buf[i]); ++i; }
        return len;
    }
    if (vn->type == VN_HOSTFILE) return -1;   // no write() in this VM

    if (vn->type == VN_PIPE) {
        // Compact first: a drained pipe reuses its buffer instead of
        // growing forever under a steady producer.
        if (vn->rpos && vn->rpos == vn->size) { vn->rpos = 0; vn->size = 0; }
        if (!vn_grow(vn, vn->size + len)) return -1;
        i = 0;
        while (i < len) { vn->data[vn->size + i] = buf[i]; ++i; }
        vn->size = vn->size + len;
        return len;
    }

    // RAM file: write at the description's position, extending as
    // needed (a gap left by seeking past the end stays zeroed).
    end = f->pos + len;
    if (!vn_grow(vn, end)) return -1;
    i = 0;
    while (i < len) { vn->data[f->pos + i] = buf[i]; ++i; }
    f->pos = end;
    if (end > vn->size) vn->size = end;
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
    i = 0;
    while (i < vn->size) { kputc(vn->data[i]); ++i; }
}

// ---- open file descriptions ----

static struct file *file_alloc(struct vnode *vn, int flags) {
    struct file *f;
    if (!(f = (struct file *)sl4b_alloc(file_cache))) return 0;
    f->vn = vn;
    f->flags = flags;
    f->pos = 0;
    f->refs = 1;
    ++vn->refs;
    if (vn->type == VN_PIPE) {
        if ((flags & 3) != C4IX_O_RDONLY) ++vn->writers;
    }
    return f;
}

static void file_unref(struct file *f) {
    --f->refs;
    if (f->refs > 0) return;
    if (f->vn->type == VN_PIPE) {
        if ((f->flags & 3) != C4IX_O_RDONLY) --f->vn->writers;
    }
    vn_release(f->vn);
    sl4b_free(file_cache, (char *)f);
}

// ---- per-task descriptor tables ----

struct file *fd_get(struct task *t, int fd) {
    if (fd < 0 || fd >= FD_MAX) return 0;
    return (struct file *)t->fds[fd];
}

int fd_install(struct task *t, struct file *f) {
    int fd;
    fd = 0;
    while (fd < FD_MAX) {
        if (!t->fds[fd]) { t->fds[fd] = (int)f; return fd; }
        ++fd;
    }
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
    if (!(f = fd_get(t, fd))) return -1;
    t->fds[fd] = 0;
    file_unref(f);
    return 0;
}

// Point newfd at oldfd's description: same flags, same position.
int fd_dup2(struct task *t, int oldfd, int newfd) {
    struct file *f;
    if (!(f = fd_get(t, oldfd))) return -1;
    if (newfd < 0 || newfd >= FD_MAX) return -1;
    if (oldfd == newfd) return newfd;
    if (t->fds[newfd]) fd_close(t, newfd);
    ++f->refs;
    t->fds[newfd] = (int)f;
    return newfd;
}

void fd_init_console(struct task *t) {
    fd_open_vnode(t, console, C4IX_O_RDONLY);   // 0
    fd_open_vnode(t, console, C4IX_O_WRONLY);   // 1
    fd_open_vnode(t, console, C4IX_O_WRONLY);   // 2
}

// Inherit the parent's table -- descriptions are shared, not copied,
// so a redirected fd 1 stays redirected in the child.
void fd_clone(struct task *dst, struct task *src) {
    struct file *f;
    int fd;
    fd = 0;
    while (fd < FD_MAX) {
        if ((f = fd_get(src, fd))) {
            ++f->refs;
            dst->fds[fd] = (int)f;
        }
        ++fd;
    }
}

void fd_closeall(struct task *t) {
    int fd;
    fd = 0;
    while (fd < FD_MAX) {
        if (t->fds[fd]) fd_close(t, fd);
        ++fd;
    }
}

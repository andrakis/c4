// bare.c -- libc's system layer (sys.h) with no C4IX under it: on the
// bare machine (libjs booting the program itself), or as a C4DOS transient.
//
// libc is built on libc4ix's syscall functions. A program linked
// statically with libc and this file instead runs where there is no
// kernel to call: what the machine does itself, it does with its own
// opcodes -- OPEN, READ and CLOS on its drives, PUTC to the console, USLP,
// EXIT -- and what it cannot, it says so (-1). Under C4DOS, which writes
// its API table into __c4dos_api when it loads the program (include/
// c4dos.h), files are written where DOS puts them (its RAM disk, or the
// home drive that DEVICE=DRIVES.SYS HOME=B: names) and read back through
// it, and exit returns to the A> prompt instead of stopping the machine.
//
// With no DOS, a file is written to drive 1 with the machine's write head
// (c4bb's DISK_W* registers), when the machine says it has them (the
// info word's C4I_DRV, asked once) and drive 1 can be written: the page
// keeps drive 1 in the browser, a store for each system, so a game's
// saves and settings outlast the page. Files are read from drive 1 first,
// then drive 0, so what was saved there is what is read.
//
// DOS's functions are called through the table as function pointers:
// c4dos.h's slot-and-stub calls are for c4cc, which has none.
//
// A file opened for reading is read whole into memory when it opens, from
// the drive or from DOS, and read and lseek work there: the machine has no
// seek, and Doom reads its WAD a lump at a time from wherever the lump is.
//
// Compiled WITHOUT -fno-builtin, as opcodes.c is, so open, read, close,
// putchar, malloc and exit here are the opcodes. Everything here is the
// subset the whole machine family has, and the write head where the
// machine announces one: no directories but the one, no processes.

int *__c4dos_api;        // the loader's, under C4DOS; 0 on the bare machine
int __c4ke_exit;         // C4KE's loader's, under C4KE: the opcode that ends a task

enum {
    DOS_MAGIC_SLOT = 0, DOS_EXIT = 1, DOS_CREATE = 2, DOS_WRITE = 3, DOS_CLOSE = 4,
    DOS_VERSION = 5, DOS_OPEN = 6, DOS_READ = 7, DOS_RCLOSE = 8,
    DOS_CONWRITE = 16, DOS_CONREAD = 17, DOS_UNLINK = 18, DOS_MAGIC = 4404292,   // include/c4dos.h's
    FD_MAX = 32,
    F_MEM = 1, F_DOSW = 3, F_DRVW = 4
};
// the machine's drives (include/c4bb.h), and the info bit that says they
// are fitted: under native c4m these addresses are ordinary memory
enum {
    BB_DRIVE = 0x13c, BB_FD = 0x124, BB_WNAME = 0x178, BB_WADDR = 0x17c, BB_WLEN = 0x180,
    BB_WCLOSE = 0x184, BB_COUNT = 0x188, BB_RO = 0x18c, BB_UNLINK = 0x1cc,
    C4I_DRV = 0x40000, NAME_MAX = 256
};

// a descriptor past the console's three: what it is; a file being written
// has DOS's handle, one being read its bytes, how many, and where it is
static int fd_kind[FD_MAX];
static int fd_h[FD_MAX];
static char *fd_buf[FD_MAX];
static int fd_len[FD_MAX];
static int fd_pos[FD_MAX];
static char *empty_env[1];
static int drive1_known, drive1_ok;
static char drvname[NAME_MAX + 4];

static int dos() { return __c4dos_api && __c4dos_api[DOS_MAGIC_SLOT] == DOS_MAGIC; }
static int dos1(int slot, int a) {
    int (*f)(int);
    if (!__c4dos_api[slot]) return 0 - 1;
    f = (int (*)(int))__c4dos_api[slot];
    return f(a);
}
static int dos3(int slot, int a, int b, int c) {
    int (*f)(int, int, int);
    if (!__c4dos_api[slot]) return 0 - 1;
    f = (int (*)(int, int, int))__c4dos_api[slot];
    return f(a, b, c);
}
// DOS v4's console slot (a function's address), or 0 when the console is
// the machine's own
static int dos_con(int slot) {
    if (!dos() || __c4dos_api[DOS_VERSION] < 4) return 0;
    return __c4dos_api[slot];
}

// Drive 1, if there is one that can be written (asked once)
static int drive1() {
    int old;
    if (!drive1_known) {
        drive1_known = 1;
        if ((__c4_info() & C4I_DRV) && *(int *)BB_COUNT > 1) {
            old = *(int *)BB_DRIVE;
            *(int *)BB_DRIVE = 1;
            drive1_ok = !*(int *)BB_RO;
            *(int *)BB_DRIVE = old;
        }
    }
    return drive1_ok;
}

// "1:" and the name, which tells the machine the drive; 0 if too long
static char *on1(char *path) {
    int i;
    while (path[0] == '.' && path[1] == '/') path = path + 2;
    while (path[0] == '/') ++path;
    drvname[0] = '1'; drvname[1] = ':';
    i = 0;
    while (path[i]) { if (i >= NAME_MAX) return 0; drvname[i + 2] = path[i]; ++i; }
    drvname[i + 2] = 0;
    return drvname;
}

static int fd_new(int kind, int h) {
    int fd;
    fd = 3;
    while (fd < FD_MAX && fd_kind[fd]) ++fd;
    if (fd == FD_MAX) return 0 - 1;
    fd_kind[fd] = kind;
    fd_h[fd] = h;
    return fd;
}

// The whole of a file, from DOS (h a handle) or the drive: its bytes, and
// their count in *n; 0 if memory runs out
static char *slurp(int fromdos, int h, int *n) {
    char *b, *nb;
    int cap, got;
    cap = 65536;
    if (!(b = (char *)malloc(cap))) return 0;
    *n = 0;
    while (1) {
        if (*n == cap) {
            if (!(nb = (char *)malloc(cap * 2))) { free(b); return 0; }
            memcpy(nb, b, *n);
            free(b);
            b = nb;
            cap = cap * 2;
        }
        got = fromdos ? dos3(DOS_READ, h, (int)(b + *n), cap - *n) : read(h, b + *n, cap - *n);
        if (got <= 0) break;
        *n = *n + got;
    }
    return b;
}

// open(2)'s flags as libc.h has them: the low two bits are the access
int uopen(char *path, int flags) {
    int h, fd, n, fromdos;
    char *b, *p1;
    if ((flags & 3) != 0) {
        // writing: where DOS puts it, or with no DOS onto drive 1
        if (dos()) {
            if ((h = dos1(DOS_CREATE, (int)path)) < 0) return 0 - 1;
            if ((fd = fd_new(F_DOSW, h)) >= 0) fd_pos[fd] = 0;
            return fd;
        }
        if (!drive1() || !(p1 = on1(path))) return 0 - 1;
        *(int *)BB_WNAME = (int)p1;
        if ((h = *(int *)BB_WNAME) < 0) return 0 - 1;
        if ((fd = fd_new(F_DRVW, h)) < 0) { *(int *)BB_WCLOSE = h; return 0 - 1; }
        fd_pos[fd] = 0;
        return fd;
    }
    // through DOS (which looks in its RAM disk and on its home drive
    // first), or with no DOS from drive 1 and then drive 0
    fromdos = 0;
    h = 0 - 1;
    if (dos() && __c4dos_api[DOS_OPEN] && (h = dos1(DOS_OPEN, (int)path)) >= 0) fromdos = 1;
    else if (!dos() && drive1() && (p1 = on1(path)) && (h = open(p1, 0)) >= 0) fromdos = 0;
    else if ((h = open(path, 0)) < 0) return 0 - 1;
    b = slurp(fromdos, h, &n);
    if (fromdos) dos1(DOS_RCLOSE, h); else close(h);
    if (!b) return 0 - 1;
    if ((fd = fd_new(F_MEM, 0)) < 0) { free(b); return 0 - 1; }
    fd_buf[fd] = b;
    fd_len[fd] = n;
    fd_pos[fd] = 0;
    return fd;
}

// The console, fd 0 to 2: under DOS v4 it is DOS's (slots 16 and 17), so
// DOS decides where it goes -- the screen, or a window of a shell's --
// and a line typed ahead of the program is the program's. Elsewhere it
// is the machine's own, READ and PUTC.
int uread(int fd, char *buf, int len) {
    int n;
    int (*con)(char *, int);
    if (fd == 0) {
        if ((con = (int (*)(char *, int))dos_con(DOS_CONREAD))) return con(buf, len);
        return read(0, buf, len);
    }
    if (fd < 3 || fd >= FD_MAX || fd_kind[fd] != F_MEM) return 0 - 1;
    n = fd_len[fd] - fd_pos[fd];
    if (n > len) n = len;
    if (n <= 0) return 0;
    memcpy(buf, fd_buf[fd] + fd_pos[fd], n);
    fd_pos[fd] = fd_pos[fd] + n;
    return n;
}

// A file being written goes one way, so lseek there only says where it
// has got to (or goes nowhere): enough for ftell, which Doom pads its
// saves by. Without it ftell said -1, every pad was a byte, and no save
// loaded back.
int ulseek(int fd, int off, int whence) {
    int at;
    if (fd >= 3 && fd < FD_MAX && (fd_kind[fd] == F_DOSW || fd_kind[fd] == F_DRVW)) {
        if ((whence == 1 || whence == 2) && off == 0) return fd_pos[fd];
        if (whence == 0 && off == fd_pos[fd]) return off;
        return 0 - 1;
    }
    if (fd < 3 || fd >= FD_MAX || fd_kind[fd] != F_MEM) return 0 - 1;
    if (whence == 0) at = off;
    else if (whence == 1) at = fd_pos[fd] + off;
    else if (whence == 2) at = fd_len[fd] + off;
    else return 0 - 1;
    if (at < 0) return 0 - 1;
    fd_pos[fd] = at;
    return at;
}

int write(int fd, char *buf, int len) {
    int i, n;
    int (*con)(char *, int);
    if (fd == 1 || fd == 2) {
        if ((con = (int (*)(char *, int))dos_con(DOS_CONWRITE))) return con(buf, len);
        i = 0;
        while (i < len) { putchar(buf[i] & 255); ++i; }
        return len;
    }
    if (fd >= 3 && fd < FD_MAX && fd_kind[fd] == F_DRVW) {
        *(int *)BB_FD = fd_h[fd];
        *(int *)BB_WADDR = (int)buf;
        *(int *)BB_WLEN = len;
        n = *(int *)BB_WLEN;
    } else if (fd >= 3 && fd < FD_MAX && fd_kind[fd] == F_DOSW) n = dos3(DOS_WRITE, fd_h[fd], (int)buf, len);
    else return 0 - 1;
    if (n > 0) fd_pos[fd] = fd_pos[fd] + n;
    return n;
}

// A file written is on its drive once it is closed, in one piece.
int uclose(int fd) {
    int r;
    if (fd < 3 || fd >= FD_MAX || !fd_kind[fd]) return 0 - 1;
    r = 0;
    if (fd_kind[fd] == F_MEM) free(fd_buf[fd]);
    else if (fd_kind[fd] == F_DRVW) { *(int *)BB_WCLOSE = fd_h[fd]; r = *(int *)BB_WCLOSE; }
    else r = dos1(DOS_CLOSE, fd_h[fd]);
    fd_kind[fd] = 0;
    return r < 0 ? 0 - 1 : 0;
}

// exit: back to DOS's prompt, the task ends under C4KE, or the machine
// stops
int uexit(int code) {
    if (dos() && __c4dos_api[DOS_EXIT]) dos1(DOS_EXIT, code);
    if (__c4ke_exit) __c4_opcode(code, __c4ke_exit);
    exit(code);
    return 0;
}

// stat: a file is there if it opens, and its size is what it reads
int ustat2(char *path, int *out, int n) {
    int fd, size, i;
    if ((fd = uopen(path, 0)) < 0) return 0 - 1;
    size = fd_len[fd];
    uclose(fd);
    i = 0;
    while (i < n) { out[i] = 0; ++i; }
    if (n > 0) out[0] = 2;               // a file (ST_TYPE)
    if (n > 1) out[1] = size;            // ST_SIZE
    if (n > 6) out[6] = 420;             // ST_MODE: 0644
    if (n > 5) out[5] = 1;               // ST_NLINK
    return 0;
}
int ustat(char *path, int *out) { return ustat2(path, out, 3); }
int ufstat(int fd, int *out, int n) {
    int i;
    if (fd < 0 || fd >= FD_MAX || (fd >= 3 && !fd_kind[fd])) return 0 - 1;
    i = 0;
    while (i < n) { out[i] = 0; ++i; }
    if (n > 0) out[0] = fd < 3 ? 3 : 2;  // the console, or a file
    if (n > 1 && fd >= 3 && fd_kind[fd] == F_MEM) out[1] = fd_len[fd];
    if (n > 1 && fd >= 3 && fd_kind[fd] != F_MEM) out[1] = fd_pos[fd];
    return 0;
}

int umsleep(int ms) { if (ms > 0) __c4_usleep(ms * 1000); return 0; }
int ugetcwd(char *buf, int len) { if (len < 2) return 0 - 1; buf[0] = '/'; buf[1] = 0; return 1; }
int uchdir(char *path) { return (path[0] == '/' || path[0] == '.') && !path[1] ? 0 : 0 - 1; }
int getpid() { return 1; }
char **uenviron() { return empty_env; }

// a file deleted: through DOS (slot 18), or with no DOS off drive 1
int uunlink(char *path) {
    char *p1;
    if (dos()) return __c4dos_api[DOS_VERSION] >= 4 ? dos1(DOS_UNLINK, (int)path) : 0 - 1;
    if (!drive1() || !(p1 = on1(path))) return 0 - 1;
    *(int *)BB_UNLINK = (int)p1;
    return *(int *)BB_UNLINK;
}

// rename: the file copied under its new name, then the old one deleted.
// Neither DOS nor the machine has a rename of its own, and Doom saves a
// game by writing temp.dsg and renaming it over doomsav0.dsg.
int urename(char *path, char *newname) {
    int fd, out, n, bad;
    if ((fd = uopen(path, 0)) < 0) return 0 - 1;
    if ((out = uopen(newname, 1)) < 0) { uclose(fd); return 0 - 1; }
    n = fd_len[fd];
    bad = n > 0 && write(out, fd_buf[fd], n) != n;
    if (uclose(out) < 0) bad = 1;
    uclose(fd);
    if (bad) return 0 - 1;
    uunlink(path);
    return 0;
}

// what the machine has no way to do
int ureaddir(char *path, int index, char *name) { return 0 - 1; }
int umkdir(char *path) { return 0 - 1; }
int udup(int fd) { return 0 - 1; }
int udup2(int oldfd, int newfd) { return 0 - 1; }
int upipe(int *fds) { return 0 - 1; }
int uexecve(char *path, char **argv, char **envp) { return 0 - 1; }
int uwaitpid(int pid, int *st, int opts) { return 0 - 1; }
int ulink(char *path, char *newpath) { return 0 - 1; }
int uftruncate(int fd, int len) { return 0 - 1; }
int vfork() { return 0 - 1; }

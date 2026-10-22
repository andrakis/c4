// libc: the POSIX calls -- unistd.h, fcntl.h, sys/stat.h, dirent.h --
// over C4IX's syscalls (libc4ix's u* functions), and errno, getopt and
// time. libc.h has the interface.
//
// C4IX's own flag values are the ones libc.h gives O_CREAT and the rest,
// O_APPEND and O_EXCL included, so open() passes them straight through.

#include "sys.h"
#include "libc.h"

int errno;

static char *err_text[40];
static int err_ready;

char *strerror(int e) {
    if (!err_ready) {
        err_ready = 1;
        err_text[0] = "Success";
        err_text[EPERM] = "Operation not permitted";
        err_text[ENOENT] = "No such file or directory";
        err_text[ESRCH] = "No such process";
        err_text[EINTR] = "Interrupted system call";
        err_text[EIO] = "Input/output error";
        err_text[ENOEXEC] = "Exec format error";
        err_text[EBADF] = "Bad file descriptor";
        err_text[ECHILD] = "No child processes";
        err_text[EAGAIN] = "Resource temporarily unavailable";
        err_text[ENOMEM] = "Cannot allocate memory";
        err_text[EACCES] = "Permission denied";
        err_text[EEXIST] = "File exists";
        err_text[ENOTDIR] = "Not a directory";
        err_text[EISDIR] = "Is a directory";
        err_text[EINVAL] = "Invalid argument";
        err_text[EMFILE] = "Too many open files";
        err_text[ENOSPC] = "No space left on device";
        err_text[ESPIPE] = "Illegal seek";
        err_text[EROFS] = "Read-only file system";
        err_text[EPIPE] = "Broken pipe";
        err_text[ERANGE] = "Numerical result out of range";
        err_text[ENOSYS] = "Function not implemented";
        err_text[ENOTEMPTY] = "Directory not empty";
    }
    if (e >= 0 && e < 40 && err_text[e]) return err_text[e];
    return "Unknown error";
}

// a C4IX syscall's -1 becomes errno E, and -1
static int fail(int e) { errno = e; return -1; }

int open(char *path, int flags, ...) {
    int fd, st[4];
    fd = uopen(path, flags & (O_ACCMODE | O_CREAT | O_TRUNC | O_NONBLOCK | O_APPEND | O_EXCL));
    if (fd >= 0) return fd;
    if (ustat(path, st) == 0) return fail((flags & O_EXCL) && (flags & O_CREAT) ? EEXIST : EACCES);
    return fail(ENOENT);
}

int creat(char *path, int mode) { return open(path, O_WRONLY | O_CREAT | O_TRUNC, mode); }
int close(int fd) { return uclose(fd) < 0 ? fail(EBADF) : 0; }
int read(int fd, char *buf, int n) { int r; r = uread(fd, buf, n); return r < 0 ? fail(EBADF) : r; }

// A RAM file seeks; anything else is a pipe, the console or a host file
int lseek(int fd, int off, int whence) {
    int r, st[ST_WORDS];
    if ((r = ulseek(fd, off, whence)) >= 0) return r;
    if (ufstat(fd, st, ST_WORDS) < 0) return fail(EBADF);
    return fail(st[ST_TYPE] == 2 ? EINVAL : ESPIPE);
}

int link(char *path, char *newpath) {
    int st[4];
    if (ulink(path, newpath) == 0) return 0;
    if (ustat(path, st) < 0) return fail(ENOENT);
    if (st[0] == 1) return fail(EPERM);
    return fail(ustat(newpath, st) == 0 ? EEXIST : ENOENT);
}

int ftruncate(int fd, int len) {
    if (len < 0) return fail(EINVAL);
    return uftruncate(fd, len) < 0 ? fail(EINVAL) : 0;
}

int truncate(char *path, int len) {
    int fd, r;
    if ((fd = open(path, O_WRONLY)) < 0) return -1;
    r = ftruncate(fd, len);
    uclose(fd);
    return r;
}

// ---- processes ----

int execve(char *path, char **argv, char **envp) {
    int st[4];
    uexecve(path, argv, envp);
    // it returns only when it failed
    return fail(ustat(path, st) == 0 ? ENOEXEC : ENOENT);
}

int execv(char *path, char **argv) { return execve(path, argv, __libc_envp()); }

// The kernel's exit code and signal, as POSIX encodes them in a status
int waitpid(int pid, int *status, int options) {
    int r, st[2];
    st[0] = 0; st[1] = 0;
    if ((r = uwaitpid(pid, st, options & WNOHANG)) < 0) return fail(ECHILD);
    if (r > 0 && status) *status = st[1] ? st[1] & 127 : (st[0] & 255) << 8;
    return r;
}

int wait(int *status) { return waitpid(-1, status, 0); }
int dup(int fd) { int r; r = udup(fd); return r < 0 ? fail(EBADF) : r; }
int dup2(int fd, int to) { int r; r = udup2(fd, to); return r < 0 ? fail(EBADF) : r; }
int pipe(int *fds) { return upipe(fds) < 0 ? fail(EMFILE) : 0; }
int unlink(char *path) { return uunlink(path) < 0 ? fail(ENOENT) : 0; }
int rmdir(char *path) { return uunlink(path) < 0 ? fail(ENOTEMPTY) : 0; }
int mkdir(char *path, int mode) {
    int st[4];
    if (ustat(path, st) == 0) return fail(EEXIST);
    return umkdir(path) < 0 ? fail(ENOENT) : 0;
}
int chdir(char *path) { return uchdir(path) < 0 ? fail(ENOENT) : 0; }
char *getcwd(char *buf, int n) { if (ugetcwd(buf, n) < 0) { errno = ERANGE; return 0; } return buf; }
int isatty(int fd) { return fd >= 0 && fd <= 2; }
unsigned int sleep(unsigned int s) { umsleep(s * 1000); return 0; }
int usleep(int us) { umsleep(us / 1000); return 0; }

// rename(from, to): C4IX renames within a directory; a move elsewhere is
// a copy and an unlink
int rename(char *from, char *to) {
    char *base, *fdir, *tdir, *buf;
    int k, fd, st[4], n;
    base = strrchr(to, '/');
    fdir = strrchr(from, '/');
    k = base ? base - to : 0;
    if ((fdir ? fdir - from : 0) == k && !strncmp(from, to, k)) {
        if (ustat(to, st) == 0) uunlink(to);
        return urename(from, base ? base + 1 : to) < 0 ? fail(ENOENT) : 0;
    }
    if (ustat(from, st) < 0 || st[0] != 2) return fail(ENOENT);
    if ((fd = uopen(from, O_RDONLY)) < 0) return fail(ENOENT);
    buf = malloc(st[1] + 1);
    n = uread(fd, buf, st[1]);
    uclose(fd);
    if ((fd = uopen(to, O_WRONLY | O_CREAT | O_TRUNC)) < 0) { free(buf); return fail(ENOENT); }
    if (n > 0) write(fd, buf, n);
    uclose(fd);
    free(buf);
    uunlink(from);
    return 0;
}

int access(char *path, int mode) {
    int st[4];
    return ustat(path, st) == 0 ? 0 : fail(ENOENT);
}

// the kernel's record (ST_*) as a struct stat
static void stat_fill(struct stat *s, int *st) {
    memset((char *)s, 0, sizeof(struct stat));
    s->st_mode = st[ST_MODE];
    s->st_size = st[ST_SIZE];
    s->st_mtime = st[ST_MTIME];
    s->st_atime = st[ST_MTIME];
    s->st_ctime = st[ST_MTIME];
    s->st_nlink = st[ST_NLINK];
    s->st_ino = st[ST_INO];
    s->st_dev = st[ST_DEV];
    s->st_blksize = 512;
    s->st_blocks = (st[ST_SIZE] + 511) / 512;
}

int stat(char *path, struct stat *s) {
    int st[ST_WORDS];
    if (ustat2(path, st, ST_WORDS) < 0) return fail(ENOENT);
    stat_fill(s, st);
    return 0;
}

int lstat(char *path, struct stat *s) { return stat(path, s); }

int fstat(int fd, struct stat *s) {
    int st[ST_WORDS];
    if (ufstat(fd, st, ST_WORDS) < 0) return fail(EBADF);
    stat_fill(s, st);
    return 0;
}

// ---- dirent ----

DIR *opendir(char *path) {
    DIR *d;
    int st[4];
    if (ustat(path, st) < 0) { errno = ENOENT; return 0; }
    if (st[0] != 1) { errno = ENOTDIR; return 0; }
    if (!(d = (DIR *)malloc(sizeof(DIR)))) { errno = ENOMEM; return 0; }
    if (!(d->ent = (struct dirent *)malloc(sizeof(struct dirent)))) { free((char *)d); errno = ENOMEM; return 0; }
    strncpy(d->path, path, 255);
    d->path[255] = 0;
    d->index = 0;
    return d;
}

struct dirent *readdir(DIR *d) {
    int r;
    r = ureaddir(d->path, d->index, d->ent->d_name);
    if (r < 0) return 0;
    ++d->index;
    d->ent->d_type = r ? DT_DIR : DT_REG;
    return d->ent;
}

int closedir(DIR *d) { free((char *)d->ent); free((char *)d); return 0; }

// ---- getopt: POSIX's, with "--" and grouped flags ----

char *optarg;
int optind = 1;
int opterr = 1;
int optopt;
static int optpos;

int getopt(int argc, char **argv, char *opts) {
    char *a, *p;
    int c;
    optarg = 0;
    if (optind >= argc) return -1;
    a = argv[optind];
    if (optpos == 0) {
        if (a[0] != '-' || a[1] == 0) return -1;
        if (a[1] == '-' && a[2] == 0) { ++optind; return -1; }
        optpos = 1;
    }
    c = a[optpos++];
    p = strchr(opts, c);
    if (!p || c == ':') {
        optopt = c;
        if (opterr && opts[0] != ':') fprintf(stderr, "%s: unknown option -- %c\n", argv[0], c);
        if (!a[optpos]) { ++optind; optpos = 0; }
        return '?';
    }
    if (p[1] == ':') {
        if (a[optpos]) optarg = a + optpos;
        else if (optind + 1 < argc) { ++optind; optarg = argv[optind]; }
        else {
            optopt = c;
            ++optind; optpos = 0;
            if (opts[0] == ':') return ':';
            if (opterr) fprintf(stderr, "%s: option requires an argument -- %c\n", argv[0], c);
            return '?';
        }
        ++optind; optpos = 0;
        return c;
    }
    if (!a[optpos]) { ++optind; optpos = 0; }
    return c;
}

// ---- time: C4IX knows how long it has been up, not the date ----

time_t time(time_t *t) {
    int s;
    s = __time() / 1000;
    if (t) *t = s;
    return s;
}

int clock() { return __time(); }

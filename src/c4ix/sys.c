//
// C4IX syscalls: the kernel side of the userland boundary.
//
// Two doors lead here, and both end in sys_dispatch:
//
//   1. The syscall gateway. Userland executes custom opcode SYS_*
//      (>= 200), which c4m does not implement, so it raises
//      TRAP_ILLOP into sched.c's handler. On plain c4 there is no
//      trap machinery at all, so libc4ix calls sys_dispatch through
//      the systable the loader injected -- same function, same
//      numbers, different door.
//
//   2. A protected task executing a host syscall opcode directly.
//      c4m raises TRAP_PM_VIOLATION instead of running it, and
//      sys_pmviolation emulates the opcode ON TOP OF the syscall
//      layer. That is what makes redirection universal: a program
//      that never heard of C4IX and just calls printf still has its
//      output go through the fd its parent gave it.
//
// Everything below fd-level goes through vfs.c. A syscall that must
// block (reading an empty pipe) parks the task and asks the trap
// handler to rewind the pc by one word, so the task re-executes the
// syscall opcode when it wakes -- its arguments are untouched on its
// own stack, so the call simply happens again.
//

#include "c4ix.h"

int sys_restart;    // set when the current syscall must be re-issued

// Park the running task on a vnode. In trap context the handler
// rewinds the pc; in kernel context the caller spins on yield, which
// costs one context switch rather than one syscall.
static int sys_block(struct vnode *vn, int pos) {
    struct task *t;
    t = sched_current();
    t->block_vn = vn;
    t->block_pos = pos;
    t->state = TS_BLOCKED;
    if (sched_in_trap()) { sys_restart = 1; return 1; }
    while (t->state == TS_BLOCKED) sched_yield();
    return 0;
}

int sys_write(int fd, char *buf, int len) {
    struct file *f;
    if (!(f = fd_get(sched_current(), fd))) return -1;
    return vfs_write(f, buf, len);
}

int sys_read(int fd, char *buf, int len) {
    struct file *f;
    struct task *t;

    t = sched_current();
    if (!(f = fd_get(t, fd))) return -1;
    while (!vfs_readable(f->vn, f->pos)) {
        if (sys_block(f->vn, f->pos)) return 0;   // trap path: restarting
        if (!(f = fd_get(t, fd))) return -1;      // table may have moved on
    }
    return vfs_read(f, buf, len);
}

// RAM files first, host files second -- the same precedence C4KE's
// loader uses, and the reason a program can be handed a "file" that
// only ever existed in memory.
int sys_open(char *path, int flags) {
    struct vnode *vn;
    struct task *t;
    int h, fd;

    t = sched_current();
    if ((vn = vfs_lookup(path))) {
        if (flags & C4IX_O_TRUNC) { vn->size = 0; }
        return fd_open_vnode(t, vn, flags);
    }
    if (flags & C4IX_O_CREAT) {
        if (!(vn = vfs_ramfile(path))) return -1;
        return fd_open_vnode(t, vn, flags);
    }
    if ((h = open(path, flags & 3)) < 0) return -1;
    if (!(vn = vn_hostfile(h))) { close(h); return -1; }
    if ((fd = fd_open_vnode(t, vn, flags)) < 0) { close(h); return -1; }
    return fd;
}

int sys_close(int fd) {
    return fd_close(sched_current(), fd);
}

int sys_dup(int fd) {
    struct task *t;
    int n;
    t = sched_current();
    if (!fd_get(t, fd)) return -1;
    n = 0;
    while (n < FD_MAX) {
        if (!fd_get(t, n)) return fd_dup2(t, fd, n);
        ++n;
    }
    return -1;
}

int sys_dup2(int oldfd, int newfd) {
    return fd_dup2(sched_current(), oldfd, newfd);
}

int sys_pipe(int *fds) {
    struct vnode *vn;
    struct task *t;
    int r, w;

    t = sched_current();
    if (!(vn = vfs_pipe())) return -1;
    if ((r = fd_open_vnode(t, vn, C4IX_O_RDONLY)) < 0) return -1;
    if ((w = fd_open_vnode(t, vn, C4IX_O_WRONLY)) < 0) { fd_close(t, r); return -1; }
    fds[0] = r;
    fds[1] = w;
    return 0;
}

// Snapshot of the index'th task, for a userland `ps`. Walking the
// list from userland is not an option -- that is kernel memory --
// so the kernel copies out a fixed-shape record instead.
int sys_taskinfo(int index, int *out) {
    struct task *t;
    char *dst;
    int i;

    t = task_first();
    i = 0;
    while (t && i < index) { t = t->next; ++i; }
    if (!t) return 0;

    out[0] = t->id;
    out[1] = t->state;
    out[2] = t->privs;
    out[3] = t->nsyscalls;
    dst = (char *)(out + 4);
    i = 0;
    while (i < TASK_NAME_MAX) { dst[i] = t->name[i]; ++i; }
    return 1;
}

int sys_chdir(char *path) { return vfs_chdir(path); }

int sys_mkdir(char *path) {
    if (!vfs_mkdir(path)) return -1;
    return 0;
}

// Build the absolute path of the working directory by walking
// parents to the root and then emitting the components in reverse.
int sys_getcwd(char *buf, int len) {
    struct vnode *chain[16];
    struct vnode *vn;
    int n, i, j, k;

    vn = vfs_cwd();
    n = 0;
    while (vn != vfs_root() && n < 16) { chain[n] = vn; ++n; vn = vn->parent; }
    if (!n) {
        if (len < 2) return -1;
        buf[0] = '/'; buf[1] = 0;
        return 1;
    }
    k = 0;
    i = n;
    while (i) {
        --i;
        if (k < len - 1) { buf[k] = '/'; ++k; }
        j = 0;
        while (chain[i]->name[j]) {
            if (k < len - 1) { buf[k] = chain[i]->name[j]; ++k; }
            ++j;
        }
    }
    buf[k] = 0;
    return k;
}

// One directory entry per call: returns 1 for a directory, 0 for a
// file, -1 past the end -- so userland never sees a kernel pointer.
int sys_readdir(char *path, int index, char *name) {
    struct vnode *dir;
    int isdir;
    if (!(dir = vfs_lookup(path))) return -1;
    if (!vfs_direntry(dir, index, name, &isdir)) return -1;
    return isdir;
}

// ---- the dispatcher ----
//
// args[0] is the first argument, args[1] the second, and so on.
// Both doors normalize into that shape before calling.
int sys_dispatch(int num, int *args) {
    struct task *t, *s;

    if ((t = sched_current())) ++t->nsyscalls;

    if (num == SYS_WRITE)  return sys_write(args[0], (char *)args[1], args[2]);
    if (num == SYS_READ)   return sys_read(args[0], (char *)args[1], args[2]);
    if (num == SYS_OPEN)   return sys_open((char *)args[0], args[1]);
    if (num == SYS_CLOSE)  return sys_close(args[0]);
    if (num == SYS_DUP)    return sys_dup(args[0]);
    if (num == SYS_DUP2)   return sys_dup2(args[0], args[1]);
    if (num == SYS_PIPE)   return sys_pipe((int *)args[0]);
    if (num == SYS_CYCLES) return __c4_cycles();
    if (num == SYS_TASKINFO) return sys_taskinfo(args[0], (int *)args[1]);
    if (num == SYS_CHDIR)  return sys_chdir((char *)args[0]);
    if (num == SYS_MKDIR)  return sys_mkdir((char *)args[0]);
    if (num == SYS_GETCWD) return sys_getcwd((char *)args[0], args[1]);
    if (num == SYS_READDIR) return sys_readdir((char *)args[0], args[1], (char *)args[2]);
    if (num == SYS_YIELD)  { sched_yield(); return 0; }
    if (num == SYS_GETPID) return t ? t->id : -1;
    if (num == SYS_SBRK)   return (int)malloc(args[0]);
    if (num == SYS_EXIT)   { task_exit(args[0]); return 0; }
    if (num == SYS_SPAWN) {
        if (!(s = task_spawn((char *)args[0], args[1], args[2]))) return -1;
        return s->id;
    }
    if (num == SYS_WAIT) {
        if (!(s = task_get(args[0]))) return -1;
        if (!sched_in_trap()) return task_wait(s);
        // In trap context blocking is a state change, not a spin:
        // park the caller and let sched_pick complete the wait when
        // the target dies -- it writes the exit code into the saved
        // accumulator, which is this syscall's return value.
        if (s->state == TS_ZOMBIE) {
            num = s->exitcode;
            task_release(s);
            return num;
        }
        t->wait_for = s->id;
        t->state = TS_WAITING;
        return 0;
    }

    kprintf("c4ix: unknown syscall %d from task %d\n", num, t ? t->id : -1);
    return -1;
}

// ---- emulating guarded host opcodes for protected tasks ----

// printf, formatted onto the fd layer. c4m's own PRTF reads the
// argument count from the word after the opcode -- the compiler
// always emits "PRTF; ADJ n" -- so the trapped returnpc points at
// that ADJ and returnpc[1] is n. Arguments sit above sp in the same
// order c4m would have read them.
//
// The supported conversions are kprintf's (%d %x %s %c %%); anything
// else prints verbatim rather than silently dropping its argument.
// Output is buffered into one write: through the fd layer this may
// be a pipe or a RAM file, and byte-at-a-time would be absurd.
static int sys_vprintf(int fd, char *fmt, int *argv, int argc) {
    char buf[512];
    char *digits, *s;
    int n, c, i, v, base, used, dstart;

    digits = "0123456789abcdef";
    n = 0;
    used = 0;
    while (*fmt) {
        if (n > 480) { sys_write(fd, buf, n); n = 0; }
        c = *fmt; ++fmt;
        if (c != '%') { buf[n] = c; ++n; }
        else if (*fmt == 0) { buf[n] = '%'; ++n; }
        else {
            c = *fmt; ++fmt;
            if (c == '%') { buf[n] = '%'; ++n; }
            else if (c == 'd' || c == 'x' || c == 'c' || c == 's') {
                v = (used < argc) ? argv[used] : 0;
                ++used;
                if (c == 's') {
                    s = (char *)v;
                    if (!s) s = "(null)";
                    while (*s) {
                        buf[n] = *s; ++n; ++s;
                        if (n > 480) { sys_write(fd, buf, n); n = 0; }
                    }
                } else if (c == 'c') { buf[n] = v; ++n; }
                else {
                    base = (c == 'x') ? 16 : 10;
                    if (v < 0 && base == 10) { buf[n] = '-'; ++n; v = -v; }
                    dstart = n;
                    if (v == 0) { buf[n] = '0'; ++n; }
                    while (v) {
                        buf[n] = digits[v - (v / base) * base]; ++n;
                        v = v / base;
                    }
                    // digits came out backwards: reverse in place
                    i = n - 1;
                    while (dstart < i) {
                        c = buf[dstart]; buf[dstart] = buf[i]; buf[i] = c;
                        ++dstart; --i;
                    }
                }
            } else { buf[n] = '%'; ++n; buf[n] = c; ++n; }
        }
    }
    if (n) sys_write(fd, buf, n);
    return n;
}

// A protected task executed a host syscall opcode. Service it on the
// kernel's terms and leave the result in the trapped accumulator.
// Anything unrecognized kills the task rather than silently letting
// it run on with a bogus result.
void sys_pmviolation(int op, int *sp, int *returnpc, int *a) {
    struct task *t;
    int argc, i;
    int args[8];

    if ((t = sched_current())) ++t->nsyscalls;

    if (op == C4IX_OP_PUTC)      { *a = sys_write(FD_STDOUT, (char *)sp, 1); return; }
    if (op == C4IX_OP_PUTS) {
        i = 0;
        while (((char *)sp[0])[i]) ++i;
        *a = sys_write(FD_STDOUT, (char *)sp[0], i);
        return;
    }
    if (op == C4IX_OP_PRTF) {
        // returnpc points at the ADJ that follows PRTF; its operand
        // is the pushed-argument count, format string included.
        argc = returnpc[1];
        if (argc > 8) argc = 8;
        // c4m reads them as t[-1], t[-2], ... from sp + argc
        i = 0;
        while (i < argc - 1) { args[i] = sp[argc - 2 - i]; ++i; }
        *a = sys_vprintf(FD_STDOUT, (char *)sp[argc - 1], args, argc - 1);
        return;
    }
    if (op == C4IX_OP_OPEN) { *a = sys_open((char *)sp[1], sp[0]); return; }
    if (op == C4IX_OP_READ) { *a = sys_read(sp[2], (char *)sp[1], sp[0]); return; }
    if (op == C4IX_OP_CLOS) { *a = sys_close(sp[0]); return; }
    if (op == C4IX_OP_MALC) { *a = (int)malloc(sp[0]); return; }
    if (op == C4IX_OP_FREE) { free((int *)sp[0]); *a = 0; return; }
    if (op == C4IX_OP_EXIT) {
        kprintf("c4ix: task %d exited via EXIT opcode, status %d\n",
            t ? t->id : -1, sp[0]);
        task_exit(sp[0]);
        return;
    }

    kprintf("c4ix: task %d used forbidden opcode %d, killing it\n",
        t ? t->id : -1, op);
    task_exit(-1);
}

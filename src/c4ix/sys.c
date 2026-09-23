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
    // Mask to the flags the host may see, but O_NONBLOCK must survive:
    // dropping it silently turns a caller's non-blocking descriptor
    // into a blocking one, and vfs_read on a VN_HOSTFILE goes straight
    // to the host read(), which halts every task in the system rather
    // than parking one. vfs_readable already answers 1 unconditionally
    // for host files, so nothing parks on this path either way, and a
    // -1/EAGAIN return is what a non-blocking reader already expects.
    if ((h = open(path, (flags & 3) | (flags & C4IX_O_NONBLOCK))) < 0) return -1;
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
    out[1] = t->parent;
    out[2] = t->state;
    out[3] = t->privs;
    out[4] = t->nsyscalls;
    out[5] = t->ntraps;
    // The running task's own total does not include the slice it is in
    // the middle of, so add it -- otherwise ps always reports itself as
    // having used nothing. Carried the same way the counter is kept.
    out[6] = t->cycles + ((t == sched_current())
        ? (__c4_cycles() - t->cycles_in) : 0);
    out[7] = t->cycles_hi;
    while (out[6] >= 1000000000) { out[6] = out[6] - 1000000000; ++out[7]; }
    dst = (char *)(out + 8);
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
    struct file *f;

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
    // Signals are the compat layer's machinery, but they are useful to
    // C4IX's own shell too: `jobs` could list a background task and
    // nothing could stop it. A C4IX program installs no handlers, so
    // for one of those this is simply "cancel that job".
    if (num == SYS_KILL)   return ck_kill(args[0], args[1]);
    if (num == SYS_YIELD)  { sched_yield(); return 0; }
    // Sleep for args[0] milliseconds. In a trap it is a state change, the
    // same one C4KE's OP_USER_SLEEP makes (c4ke.c): park on the clock and
    // let the scheduler run everyone else, and nap when nobody is ready.
    // It completes on waking, so nothing re-executes.
    // Bytes waiting on fd, without ever blocking: the desktop reads every
    // terminal's pipe from one loop. 0 means empty with a writer still
    // there, -1 end of file (or a bad fd).
    if (num == SYS_AVAIL) {
        if (!t || !(f = fd_get(t, args[0]))) return -1;
        if (f->vn->type == VN_PIPE) {
            if (f->vn->size > f->vn->rpos) return f->vn->size - f->vn->rpos;
            return f->vn->writers ? 0 : -1;
        }
        if (f->vn->type == VN_CONSOLE) return con_poll() ? 1 : 0;
        if (f->vn->type == VN_RAMFILE) return f->vn->size > f->pos ? f->vn->size - f->pos : -1;
        return 1;
    }
    if (num == SYS_INTR) return sched_intr_below(task_get(args[0]));
    if (num == SYS_STAT)   return vfs_stat((char *)args[0], (int *)args[1]);
    if (num == SYS_UNLINK) return vfs_unlink((char *)args[0]);
    if (num == SYS_RENAME) return vfs_rename((char *)args[0], (char *)args[1]);
    if (num == SYS_CLOEXEC) {
        if (!t || !fd_get(t, args[0])) return -1;
        t->fdcloexec[args[0]] = args[1] ? 1 : 0;
        return 0;
    }
    if (num == SYS_SLEEP) {
        if (!t || args[0] <= 0) return 0;
        if (!sched_in_trap()) {
            num = __time() + args[0];
            while (__time() - num < 0) sched_yield();
            return 0;
        }
        t->ck_wake = __time() + args[0];
        t->state = TS_SLEEPING;
        return 0;
    }
    if (num == SYS_GETPID) return t ? t->id : -1;
    if (num == SYS_SBRK)   return (int)malloc(args[0]);
    if (num == SYS_EXIT)   { task_exit(args[0]); return 0; }
    if (num == SYS_SPAWN) {
        if (!(s = task_spawn((char *)args[0], args[1], args[2]))) return -1;
        return s->id;
    }
    if (num == SYS_WAIT) {
        // The target may have been swept by the idle reaper before the
        // parent got round to waiting; its exit code outlives it.
        if (!(s = task_get(args[0]))) {
            if (task_ghost(args[0], &num)) return num;
            return -1;
        }
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
// The conversion set has to be REAL, not kprintf's five. c4m's PRTF
// hands the format string to the host's printf, so a program written
// for c4m expects everything the host supports; the moment such a
// program runs behind protected mode, this function is what it gets
// instead. C4KE's ps alone uses %3ld, %4ld.%03d, %2d, %3d%% and %*s,
// and %*s is the one that proves the point -- an unrecognised spec
// that does not CONSUME its width argument shifts every conversion
// after it, so the output is not merely misaligned, it is wrong.
//
// Supported: flags '-' and '0' (and '+', ' ', '#' accepted and
// ignored), a width or '*', a precision or '.*', the length
// modifiers l/ll/h/z (every integer here is one word), and the
// conversions d i u x X c s %. libc4ix's uformat is the same parser
// for userland -- keep the two in step.
//
// Output is buffered into one write: through the fd layer this may
// be a pipe or a RAM file, and byte-at-a-time would be absurd.

enum { PF_BUF = 512, PF_HIGH = 480 };

static char pf_buf[PF_BUF];
static int  pf_n;
static int  pf_fd;
static int  pf_out;      // characters emitted, printf's return value

static void pf_flush() {
    if (pf_n) { sys_write(pf_fd, pf_buf, pf_n); pf_n = 0; }
}

static void pf_ch(int c) {
    pf_buf[pf_n] = c;
    ++pf_n;
    ++pf_out;
    if (pf_n >= PF_HIGH) pf_flush();
}

static void pf_pad(int c, int n) {
    while (n > 0) { pf_ch(c); --n; }
}

// Digits of V in BASE, most significant first, into BUF; returns the
// length. Base 16 is treated as UNSIGNED: C4 has no unsigned type, so
// the shift is masked back down to 60 bits rather than sign-extending.
static int pf_digits(char *buf, int v, int base) {
    char tmp[72];
    char *ds;
    int n, i, mask;

    ds = "0123456789abcdef";
    mask = (1 << 60) - 1;
    n = 0;
    if (!v) { tmp[0] = '0'; n = 1; }
    while (v) {
        if (base == 16) { tmp[n] = ds[v & 15]; v = (v >> 4) & mask; }
        else { tmp[n] = ds[v - (v / base) * base]; v = v / base; }
        ++n;
    }
    i = 0;
    while (n) { --n; buf[i] = tmp[n]; ++i; }
    return i;
}

static int sys_vprintf(int fd, char *fmt, int *argv, int argc) {
    char num[80];
    char *s;
    int c, v, used, width, prec, left, zero, len, neg, i, base, upper;

    pf_fd = fd;
    pf_n = 0;
    pf_out = 0;
    used = 0;

    while (*fmt) {
        c = *fmt; ++fmt;
        if (c != '%') { pf_ch(c); continue; }
        if (!*fmt) { pf_ch('%'); break; }

        // flags
        left = 0; zero = 0;
        while (1) {
            c = *fmt;
            if (c == '-') { left = 1; ++fmt; }
            else if (c == '0') { zero = 1; ++fmt; }
            else if (c == '+' || c == ' ' || c == '#') { ++fmt; }
            else break;
        }

        // width, possibly from an argument -- a negative one means
        // left-justify, which is how ps asks for it (ps.c:312 computes
        // a width of -2)
        width = 0;
        if (*fmt == '*') {
            ++fmt;
            width = (used < argc) ? argv[used] : 0; ++used;
            if (width < 0) { left = 1; width = -width; }
        } else {
            while (*fmt >= '0' && *fmt <= '9') {
                width = width * 10 + (*fmt - '0'); ++fmt;
            }
        }

        // precision: minimum digits for integers, maximum for strings
        prec = -1;
        if (*fmt == '.') {
            ++fmt; prec = 0;
            if (*fmt == '*') {
                ++fmt;
                prec = (used < argc) ? argv[used] : 0; ++used;
                if (prec < 0) prec = -1;
            } else {
                while (*fmt >= '0' && *fmt <= '9') {
                    prec = prec * 10 + (*fmt - '0'); ++fmt;
                }
            }
        }

        // length modifiers: every integer here is one machine word
        while (*fmt == 'l' || *fmt == 'h' || *fmt == 'z') ++fmt;

        c = *fmt;
        if (!c) { pf_ch('%'); break; }
        ++fmt;

        if (c == '%') { pf_pad(' ', left ? 0 : width - 1); pf_ch('%');
                        pf_pad(' ', left ? width - 1 : 0); continue; }

        // Anything unrecognised prints verbatim AND CONSUMES NOTHING,
        // which keeps the remaining arguments aligned.
        if (c != 'd' && c != 'i' && c != 'u' && c != 'x' && c != 'X'
            && c != 'c' && c != 's') {
            pf_ch('%'); pf_ch(c);
            continue;
        }

        v = (used < argc) ? argv[used] : 0; ++used;

        if (c == 'c') {
            if (!left) pf_pad(' ', width - 1);
            pf_ch(v);
            if (left) pf_pad(' ', width - 1);
            continue;
        }

        if (c == 's') {
            s = (char *)v;
            if (!s) s = "(null)";
            len = 0;
            while (s[len] && (prec < 0 || len < prec)) ++len;
            if (!left) pf_pad(' ', width - len);
            i = 0;
            while (i < len) { pf_ch(s[i]); ++i; }
            if (left) pf_pad(' ', width - len);
            continue;
        }

        // integers
        upper = (c == 'X');
        base = (c == 'x' || c == 'X') ? 16 : 10;
        neg = 0;
        if (base == 10 && v < 0) { neg = 1; v = -v; }
        len = pf_digits(num, v, base);
        if (upper) {
            i = 0;
            while (i < len) {
                if (num[i] >= 'a' && num[i] <= 'f') num[i] = num[i] - 32;
                ++i;
            }
        }
        // an explicit precision is a minimum digit count, and it
        // overrides zero padding (C's rule)
        if (prec >= 0) { zero = 0; while (len < prec) { i = len; while (i) { num[i] = num[i - 1]; --i; } num[0] = '0'; ++len; } }
        i = len + neg;
        if (!left && !zero) pf_pad(' ', width - i);
        if (neg) pf_ch('-');
        if (!left && zero) pf_pad('0', width - i);
        i = 0;
        while (i < len) { pf_ch(num[i]); ++i; }
        if (left) pf_pad(' ', width - len - neg);
    }

    pf_flush();
    return pf_out;
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
    // Verbatim, not synthesized. Programs branch on these bits --
    // C4KE's ps patches its own code differently when C4I_C4 is set --
    // so the only safe answer is what the machine actually is.
    if (op == C4IX_OP_INFO) { *a = host_info(); return; }
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

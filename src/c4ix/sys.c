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
//      output go through sys_write.
//
// The fd layer is deliberately minimal at X2: 0/1/2 are the console,
// higher descriptors are host files. Per-task tables, vnodes and
// pipes are X3's job -- this is the interface they will implement.
//

#include "c4ix.h"

static int fd_open[FD_MAX];      // 1 if a host file is open on this fd
static int fd_host[FD_MAX];      // host descriptor behind it

int sys_console_fd(int fd) {
    if (fd == FD_STDOUT) return 1;
    if (fd == FD_STDERR) return 1;
    return 0;
}

int sys_write(int fd, char *buf, int len) {
    int i;
    if (sys_console_fd(fd)) {
        i = 0;
        while (i < len) { kputc(buf[i]); ++i; }
        return len;
    }
    if (fd < 0 || fd >= FD_MAX) return -1;
    if (!fd_open[fd]) return -1;
    // The VM has no write() syscall, so host files stay read-only at
    // X2; X3's RAM-FS vnodes are where writable files arrive.
    return -1;
}

int sys_read(int fd, char *buf, int len) {
    if (fd == FD_STDIN) return read(0, buf, len);
    if (fd < 0 || fd >= FD_MAX) return -1;
    if (!fd_open[fd]) return -1;
    return read(fd_host[fd], buf, len);
}

int sys_open(char *path, int flags) {
    int h, fd;
    if ((h = open(path, flags)) < 0) return -1;
    fd = 3;
    while (fd < FD_MAX) {
        if (!fd_open[fd]) {
            fd_open[fd] = 1;
            fd_host[fd] = h;
            return fd;
        }
        ++fd;
    }
    close(h);
    return -1;
}

int sys_close(int fd) {
    if (sys_console_fd(fd)) return 0;
    if (fd < 0 || fd >= FD_MAX) return -1;
    if (!fd_open[fd]) return -1;
    close(fd_host[fd]);
    fd_open[fd] = 0;
    return 0;
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
static int sys_vprintf(int fd, char *fmt, int *argv, int argc) {
    char buf[32];
    char *digits, *s;
    int n, c, i, v, base, neg, used;

    digits = "0123456789abcdef";
    n = 0;
    used = 0;
    while (*fmt) {
        c = *fmt; ++fmt;
        if (c != '%') { n = n + sys_write(fd, fmt - 1, 1); }
        else if (*fmt == 0) { n = n + sys_write(fd, "%", 1); }
        else {
            c = *fmt; ++fmt;
            if (c == '%') { n = n + sys_write(fd, "%", 1); }
            else if (c == 'd' || c == 'x' || c == 'c' || c == 's') {
                if (used >= argc) { n = n + sys_write(fd, "(missing)", 9); }
                else {
                    v = argv[used]; ++used;
                    if (c == 's') {
                        s = (char *)v;
                        if (!s) s = "(null)";
                        i = 0;
                        while (s[i]) ++i;
                        n = n + sys_write(fd, s, i);
                    } else if (c == 'c') {
                        buf[0] = v;
                        n = n + sys_write(fd, buf, 1);
                    } else {
                        base = (c == 'x') ? 16 : 10;
                        neg = 0;
                        if (v < 0 && base == 10) { neg = 1; v = -v; }
                        i = 0;
                        if (v == 0) { buf[i] = '0'; ++i; }
                        while (v) { buf[i] = digits[v - (v / base) * base]; ++i; v = v / base; }
                        if (neg) { n = n + sys_write(fd, "-", 1); }
                        while (i) { --i; n = n + sys_write(fd, buf + i, 1); }
                    }
                }
            } else {
                n = n + sys_write(fd, "%", 1);
                n = n + sys_write(fd, fmt - 1, 1);
            }
        }
    }
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

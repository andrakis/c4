//
// C4IX init: the first task, and the kernel's command line.
//
// With no arguments it boots to a shell, because that is what anyone
// running this actually wants; `--demo` asks for the guided tour and
// `--help` lists the rest. init_usage() is the authority on all of
// it -- keep the two in step.
//
// The tour has three acts:
//   1. Cooperative ping/pong -- two tasks yielding explicitly. Runs
//      under sched_lock so the interleaving is pure round-robin on
//      both hosts and the output pins exactly.
//   2. Preemption proof (c4m only) -- two busy tasks that never
//      yield, each sampling the other's progress counter halfway
//      through its own loop. Only preemption can interleave them, so
//      "saw the other side mid-run" is the boolean that gets printed
//      -- no interleaving-sensitive output, the pin survives codegen
//      changes shifting cycle counts.
//   3. Spawn -- load a .c4r from the host filesystem (kernel argv[1]
//      if present), run it as a task, wait for its exit code.
//

#include "c4ix.h"

static int pp_rounds;

static int ping_task(int argc, int argv) {
    int i;
    i = 0;
    while (i < argc) {
        kprintf("ping %d\n", i);
        sched_yield();
        ++i;
    }
    return 100 + argc;
}

static int pong_task(int argc, int argv) {
    int i;
    i = 0;
    while (i < argc) {
        kprintf("pong %d\n", i);
        sched_yield();
        ++i;
    }
    return 200 + argc;
}

// busy tasks: no yields, just work and one mid-run sample of the
// other side's progress
static int busy_a_n;
static int busy_b_n;
static int busy_a_saw;
static int busy_b_saw;

enum { BUSY_SPIN = 60000 };

static int busy_a(int argc, int argv) {
    int i;
    i = 0;
    while (i < BUSY_SPIN) {
        busy_a_n = busy_a_n + 1;
        if (i == BUSY_SPIN / 2) busy_a_saw = busy_b_n;
        ++i;
    }
    return 0;
}

static int busy_b(int argc, int argv) {
    int i;
    i = 0;
    while (i < BUSY_SPIN) {
        busy_b_n = busy_b_n + 1;
        if (i == BUSY_SPIN / 2) busy_b_saw = busy_a_n;
        ++i;
    }
    return 0;
}

// ---- X3: the IO layer, doing a shell's job by hand ----

// A RAM file is ordinary storage behind the same fd calls: open it,
// write it, close it, open it again and read it back.
static void io_ramfile() {
    char buf[64];
    int fd, n;

    if ((fd = sys_open("/ram/note", C4IX_O_WRONLY + C4IX_O_CREAT)) < 0) {
        kputs("init: cannot create /ram/note\n");
        return;
    }
    sys_write(fd, "written to a ram file\n", 22);
    sys_close(fd);

    if ((fd = sys_open("/ram/note", C4IX_O_RDONLY)) < 0) {
        kputs("init: cannot reopen /ram/note\n");
        return;
    }
    n = sys_read(fd, buf, 64);
    sys_close(fd);
    kprintf("init: read %d bytes back: ", n);
    sys_write(FD_STDOUT, buf, n);
}

// Redirection, exactly as a shell does it and with no fork in sight:
// point our own fd 1 at a file, spawn, then put fd 1 back. The child
// inherits the table, so its output lands in the file -- and the
// child here is c4ix-hello.c4r, which calls printf directly and has
// never heard of C4IX. Under protected mode that printf traps and
// the kernel writes it wherever fd 1 now points.
static void io_redirect(char *prog) {
    struct task *t;
    int fd, saved;

    if ((fd = sys_open("/ram/out", C4IX_O_WRONLY + C4IX_O_CREAT + C4IX_O_TRUNC)) < 0) {
        kputs("init: cannot create /ram/out\n");
        return;
    }
    // Announce this BEFORE the redirection, not after. From here until
    // the child exits, everything it prints goes into the file -- so
    // if the caller's program list is short enough that something
    // interactive lands in this slot, the terminal falls silent with
    // no explanation and looks hung. It is not hung; the child is
    // waiting to be typed at, and its prompt went into /ram/out.
    kprintf("init: running '%s' with fd 1 redirected into /ram/out\n", prog);
    saved = sys_dup(FD_STDOUT);
    sys_dup2(fd, FD_STDOUT);
    t = task_spawn(prog, 1, 0);
    sys_dup2(saved, FD_STDOUT);      // restore before anything prints
    sys_close(saved);
    sys_close(fd);
    if (!t) { kprintf("init: spawn of '%s' failed\n", prog); return; }
    task_wait(t);

    kprintf("init: '%s' ran with fd 1 redirected; /ram/out holds %d bytes:\n",
        prog, vfs_size("/ram/out"));
    vfs_dump("/ram/out");
    // Without protected mode a raw printf never reaches the kernel,
    // so there is nothing to redirect. Saying so beats an empty file
    // that looks like a bug.
    if (vfs_size("/ram/out") == 0)
        kputs("init: (empty: this host has no protected mode, so a raw printf bypasses fd 1)\n");
}

// A pipeline: writer's fd 1 and reader's fd 0 are the two ends of
// one pipe. The reader blocks on the empty pipe until the writer
// produces -- and its read syscall is restarted, not resumed.
static void io_pipeline(char *wprog, char *rprog) {
    struct task *w, *r;
    int p[2], saved_out, saved_in;
    char *wargv[4];

    if (sys_pipe(p) < 0) { kputs("init: pipe failed\n"); return; }

    wargv[0] = wprog;
    wargv[1] = "piped";
    wargv[2] = "through";
    wargv[3] = 0;

    saved_out = sys_dup(FD_STDOUT);
    sys_dup2(p[1], FD_STDOUT);
    w = task_spawn(wprog, 3, (int)wargv);
    sys_dup2(saved_out, FD_STDOUT);
    sys_close(saved_out);

    // The write end must be gone from OUR table before the reader is
    // spawned, or the reader inherits a copy of it and then waits
    // forever for an end of file it is itself holding open. Every
    // shell has this bug once.
    sys_close(p[1]);

    saved_in = sys_dup(FD_STDIN);
    sys_dup2(p[0], FD_STDIN);
    r = task_spawn(rprog, 1, 0);
    sys_dup2(saved_in, FD_STDIN);
    sys_close(saved_in);
    sys_close(p[0]);

    if (!w || !r) { kputs("init: pipeline spawn failed\n"); return; }
    task_wait(w);
    task_wait(r);
}

// X4: hand the whole job over to a shell. init stops being the thing
// that wires up pipes and redirections by hand -- it just starts
// c4ix-sh on a script, and the shell does all of it through
// syscalls, from userland, behind protected mode.
static void init_shell(char *shell, char *script) {
    struct task *t;
    char *sargv[3];

    sargv[0] = shell;
    sargv[1] = script;
    sargv[2] = 0;

    kprintf("init: --- X4: handing off to %s %s ---\n", shell, script);
    if (!(t = task_spawn(shell, 2, (int)sargv))) {
        kprintf("init: cannot start %s\n", shell);
        return;
    }
    kprintf("init: shell exited %d after %d syscalls\n",
        task_wait(t), task_last_syscalls);
}

static void init_io(char **av, int argc) {
    kputs("init: --- X3: files, redirection, pipes ---\n");
    io_ramfile();
    if (argc > 1) io_redirect(av[1]);
    if (argc > 4) io_pipeline(av[3], av[4]);
    if (argc > 6) init_shell(av[5], av[6]);
}

// Exact match, because the options are words rather than clusters of
// letters and a prefix match would quietly accept nonsense.
static int init_is(char *s, char *opt) {
    while (*s) {
        if (*s != *opt) return 0;
        ++s;
        ++opt;
    }
    return *opt == 0;
}

// The demonstrations used to be what you got for asking for nothing,
// which had the common case behind the uncommon one -- and worse, the
// demonstrations take their programs POSITIONALLY, so a short list
// put whatever you named into every role in turn. Booting to a shell
// now needs no arguments at all, and the tour is behind --demo.
static void init_usage() {
    kputs("c4ix -- a small Unix-like kernel for the C4 virtual machine\n");
    kputs("\n");
    kputs("usage: c4ix.c4r [OPTION] [PROGRAM [ARGUMENT]...]\n");
    kputs("\n");
    kputs("  (no arguments)     boot to an interactive shell\n");
    kputs("  PROGRAM [ARG]...   boot, run PROGRAM with ARGs, shut down\n");
    kputs("  --demo IMAGE...    run the built-in demonstrations, below\n");
    kputs("  -h, --help         print this and shut down\n");
    kputs("  -q                 accepted and ignored, so older command\n");
    kputs("                     lines keep working\n");
    kputs("\n");
    kputs("PROGRAM is an image path as the host filesystem sees it. The\n");
    kputs("SHELL expands a bare name to c4ix-NAME.c4r or NAME.c4r; init\n");
    kputs("does not, so name the image in full here.\n");
    kputs("\n");
    kputs("--demo takes six images and uses each for a fixed role:\n");
    kputs("  1  calls printf directly -- proves redirection reaches a\n");
    kputs("     program that has never heard of C4IX\n");
    kputs("  2  uses libc4ix syscalls\n");
    kputs("  3  writer and 4 reader of a pipeline\n");
    kputs("  5  a shell, and 6 a script for it to run\n");
    kputs("Fewer than six still runs the stages it has arguments for,\n");
    kputs("so a short list puts one image into several roles.\n");
    kputs("\n");
    kputs("  c4ix.c4r --demo c4ix-hello.c4r c4ix-uhello.c4r c4ix-echo.c4r \\\n");
    kputs("           c4ix-wc.c4r c4ix-sh.c4r src/c4ix/user/test.sh\n");
}

// The guided tour. argc/av are shifted past --demo, so av[1] is the
// first image and every stage below indexes exactly as documented.
static int init_demo(int argc, char **av) {
    struct task *a, *b;
    int ra, rb, n;

    kprintf("init: c4ix init v1 on %s\n", host_name());

    // act 1: cooperative round-robin, preemption masked for an exact
    // interleaving pin on both hosts
    sched_lock();
    pp_rounds = 3;
    a = task_create("ping", (int)&ping_task, pp_rounds, 0);
    b = task_create("pong", (int)&pong_task, pp_rounds, 0);
    kprintf("init: ping id %d, pong id %d, tasks %d\n", a->id, b->id, task_count());
    ra = task_wait(a);
    rb = task_wait(b);
    kprintf("init: ping exited %d, pong exited %d\n", ra, rb);
    sched_unlock();

    // act 2: preemption, where the host has a cycle interrupt
    if (host_type() == HOST_C4M) {
        a = task_create("busy_a", (int)&busy_a, 0, 0);
        b = task_create("busy_b", (int)&busy_b, 0, 0);
        ra = task_wait(a);
        rb = task_wait(b);
        kprintf("init: busy done %d %d, preempted %d %d\n",
            busy_a_n == BUSY_SPIN, busy_b_n == BUSY_SPIN,
            busy_a_saw > 0, busy_b_saw > 0);
    } else {
        kprintf("init: no preemption on this host, skipping busy demo\n");
    }

    // act 3: spawn programs from the host filesystem. Each runs as
    // userland -- behind protected mode where the host provides it --
    // so every one of their syscalls passes through sys.c, whether
    // the program asks through libc4ix or just calls printf.
    // Kernel argv: [1] a raw-printf program, [2] a libc4ix program,
    // [3] and [4] the two halves of the X3 pipeline. The first two
    // run standalone here; the rest are driven by init_io below.
    if (argc > 1) {
        n = 1;
        while (n < argc && n < 3) {
            if ((a = task_spawn(av[n], argc - n, (int)(av + n)))) {
                kprintf("init: spawned '%s' as task %d (%s)\n", a->name, a->id,
                    a->privs == PRIV_USER ? "protected" : "unprotected");
                ra = task_wait(a);
                kprintf("init: '%s' exited %d after %d syscalls\n",
                    av[n], ra, task_last_syscalls);
            } else {
                kprintf("init: spawn of '%s' failed\n", av[n]);
            }
            ++n;
        }
        init_io(av, argc);
    }

    return 0;
}

int init_main(int argc, int argv) {
    struct task *a;
    char **av;
    char *shargv[2];
    int i;

    av = (char **)argv;
    i = 1;

    if (i < argc) {
        if (init_is(av[i], "-h") || init_is(av[i], "--help")) {
            init_usage();
            return 0;
        }
        // Shift past the flag so the stages index from av[1] exactly
        // as the help text describes.
        if (init_is(av[i], "--demo")) return init_demo(argc - i, av + i);
        // -q used to mean "skip the demonstrations". They are opt-in
        // now, so it means nothing -- but silently accepting it keeps
        // older command lines and older habits working.
        if (init_is(av[i], "-q")) ++i;
    }

    // Nothing left to run: the default, and the reason the default is
    // worth having.
    if (i >= argc) {
        shargv[0] = "c4ix-sh.c4r";
        shargv[1] = 0;
        if (!(a = task_spawn(shargv[0], 1, (int)shargv))) {
            kprintf("init: cannot start '%s'\n", shargv[0]);
            return 1;
        }
        task_wait(a);
        return 0;
    }

    if (!(a = task_spawn(av[i], argc - i, (int)(av + i)))) {
        kprintf("init: spawn of '%s' failed\n", av[i]);
        return 1;
    }
    task_wait(a);
    return 0;
}

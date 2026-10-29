// C4IX userland: cc. Compiles C with c4sc -- c4lc, the optimising C
// compiler, compiled to an image -- where `c4sp c4lc.lisp` runs the same
// compiler interpreted, many times slower (docs/c4sc-design.md).
//
//   cc [-O] [-c] [-conforming] [-fno-builtin] [-fpic] [-mfuse] [-v]
//      [-stack bytes] [-I dir] [-D name] [-l lib] [-o out] file.c...
//
// c4lc's own preprocessor is always on (-P). After any -I given it
// searches /usr/include: C4IX's headers, and the repo's own beside them
// (u0.h, c4dos.h and the C versions of stdio.h and the rest), so the
// repo's programs (/usr/src/tests/mandel.c, ...) build as they are.
// Those headers ask __c4cc__ / C4CC whether they are building for the
// VM rather than natively, and cc always is, so both are defined -- as
// the Makefile does when c4lc builds them. Without -o a program is NAME.c4r in
// the working directory, NAME the first source's file name less .c; with
// -c each source becomes its own NAME.c4o.
//
// One source and no -l is a whole program, written by c4sc as c4lc
// writes one: a program that calls only c4's own library (printf, open,
// malloc, ...) needs nothing more. More sources, or -l, are compiled to
// objects in /tmp and joined by c4rlink:
//   -l c4ix          /usr/lib/libc4ix.c4l, linked in: C4IX's own calls
//   -l arc           /usr/lib/libarc.c4l (tar, gzip, crc32)
//   -l /lib/libc.c4s a shared library, loaded when the program starts
//                    (its callers are built -fno-builtin)
//
// -v prints each command before running it. The tools run on a task's
// default stack, 256 KB, and -stack gives more. c4sc needs a few KB of
// it: C4KE's kernel, src/c4ke/c4ke.c, peaks at 3.4 KB (2026-10-25). It
// spends about 108 bytes a struct member and 124 an else-if arm or a
// level of nesting, so only a generated source should need -stack. The
// 4 MB cc gave before was for a c4lc bug, g:resolvecode recursing once
// per instruction (fixed in 29cbac9).
//
// When the kernel stops a tool -- a stack overrun, a bad opcode -- it
// says why on the console, and a Terminal is not the console: cc
// finds that line in /var/log/kernel and says it too, rather than only that
// the source did not compile.
#include "c4ix_user.h"

enum { MAXA = 64 };

static char *KLOG = "/var/log/kernel";

static char *flags[MAXA];        // passed to c4sc as they are
static int   nflags;
static char *srcs[MAXA];
static int   nsrc;
static char *libs[MAXA];
static int   nlib;
static int   verbose;
static int   stack;               // bytes, for each tool cc runs; 0 the default

static char *cat3(char *a, char *b, char *c) {
    char *p, *q;
    p = (char *)ualloc(ustrlen(a) + ustrlen(b) + ustrlen(c) + 1);
    q = ustrcpy(p, a);
    q = ustrcpy(q - 1, b);
    ustrcpy(q - 1, c);
    return p;
}

static int ends(char *s, char *suffix) {
    int n, m;
    n = ustrlen(s); m = ustrlen(suffix);
    return n >= m && !ustrcmp(s + n - m, suffix);
}

// A source's file name, less its directory and its .c, then ext.
static char *named(char *src, char *ext) {
    char *p, *b;
    int i, n;
    b = src;
    i = 0;
    while (src[i]) { if (src[i] == '/') b = src + i + 1; ++i; }
    n = ustrlen(b);
    if (ends(b, ".c")) n = n - 2;
    p = (char *)ualloc(n + ustrlen(ext) + 1);
    i = 0;
    while (i < n) { p[i] = b[i]; ++i; }
    ustrcpy(p + n, ext);
    return p;
}

static int prefix(char *s, char *p) {
    while (*p && *s == *p) { ++s; ++p; }
    return !*p;
}

// What the kernel said about task pid in /var/log/kernel past its first
// `from` bytes ("c4ix: task 8 (c4sc) overran its 1048576-byte stack;
// killing it"), said again here: 0 if it said nothing, 2 if it was a
// stack overrun, 1 for anything else. Only what was logged after the
// tool started counts: an older line may be about another task.
static int why(int pid, int from) {
    int st[4], fd, n, k, id, said;
    char *log, *p, *q;
    if (from < 0 || ustat(KLOG, st) < 0 || st[1] <= from) return 0;
    if (!(log = (char *)ualloc(st[1] + 1))) return 0;
    if ((fd = uopen(KLOG, 0)) < 0) return 0;
    k = 0;
    while (k < st[1] && (n = uread(fd, log + k, st[1] - k)) > 0) k = k + n;
    uclose(fd);
    log[k] = 0;
    said = 0;
    p = log + from;
    while (*p) {
        q = p;
        while (*q && *q != 10) ++q;
        if (prefix(p, "c4ix: task ")) {
            id = 0; n = 11;
            while (p[n] >= '0' && p[n] <= '9') { id = id * 10 + p[n] - '0'; ++n; }
            if (id == pid && p[n] == ' ') {
                k = *q; *q = 0;
                ufprintf(STDERR, "cc: the kernel said: %s\n", p + 6);   // less its "c4ix: "
                *q = k;
                said = 1;
                while (p < q && !prefix(p, "overran")) ++p;
                if (p < q) said = 2;
            }
        }
        p = *q ? q + 1 : q;
    }
    return said;
}

// Runs a tool from /bin, as b4ke finds one, and answers its exit code.
static int run(char *prog, int argc, char **argv) {
    int pid, i, code, from, st[4];
    from = ustat(KLOG, st) < 0 ? -1 : st[1];   // the log so far: not about this run
    if (verbose) {
        i = 0;
        while (i < argc) { uprintf(i ? " %s" : "%s", argv[i]); ++i; }
        uprintf("\n");
    }
    pid = spawn(cat3("/bin/", prog, ""), argc, argv);
    if (pid < 0) pid = spawn(cat3(prog, ".c4r", ""), argc, argv);
    if (pid < 0) pid = spawn(cat3("c4ix-", prog, ".c4r"), argc, argv);
    if (pid < 0) { ufprintf(STDERR, "cc: cannot run %s\n", prog); return 1; }
    code = uwait(pid);
    // a tool the kernel stopped: say what it said
    if (code < 0 && why(pid, from) == 2)
        ufprintf(STDERR, "cc: %s ran out of stack; -stack BYTES gives it more\n", prog);
    return code;
}

// c4sc compile ... src out, -c for an object.
static int compile(char *src, char *out, int obj) {
    char **av;
    int n, i;
    av = (char **)ualloc((nflags + 18) * sizeof(char *));
    n = 0;
    av[n] = "c4sc"; ++n;
    av[n] = "-c"; ++n;                // c4sp's heap, in cells: c4sc's, not cc's -c
    av[n] = "400000"; ++n;
    av[n] = "compile"; ++n;
    av[n] = "-P"; ++n;
    i = 0;
    while (i < nflags) { av[n] = flags[i]; ++n; ++i; }
    av[n] = "-I"; ++n;
    av[n] = "/usr/include"; ++n;
    av[n] = "-D"; ++n;
    av[n] = "C4CC=1"; ++n;
    av[n] = "-D"; ++n;
    av[n] = "__c4cc__=1"; ++n;
    if (obj) { av[n] = "-c"; ++n; }
    av[n] = src; ++n;
    av[n] = out; ++n;
    av[n] = 0;
    return run("c4sc", n, av);
}

static int usage() {
    ufprintf(STDERR, "usage: cc [-O] [-c] [-conforming] [-fno-builtin] [-fpic] [-mfuse] [-v]\n");
    ufprintf(STDERR, "          [-stack bytes] [-I dir] [-D name] [-l lib] [-o out] file.c...\n");
    return 2;
}

int main(int argc, char **argv) {
    int i, k, code, objonly, link, n;
    char *a, *out, **objs, **av;

    out = 0; objonly = 0;
    i = 1;
    while (i < argc) {
        a = argv[i];
        if (!ustrcmp(a, "-o") && i + 1 < argc) { out = argv[i + 1]; i = i + 2; }
        else if ((!ustrcmp(a, "-I") || !ustrcmp(a, "-D")) && i + 1 < argc && nflags + 2 < MAXA) {
            flags[nflags] = a; flags[nflags + 1] = argv[i + 1]; nflags = nflags + 2; i = i + 2;
        } else if ((a[0] == '-' && (a[1] == 'I' || a[1] == 'D') && a[2]) && nflags + 2 < MAXA) {
            flags[nflags] = a[1] == 'I' ? "-I" : "-D"; flags[nflags + 1] = a + 2; nflags = nflags + 2; ++i;
        } else if (!ustrcmp(a, "-l") && i + 1 < argc && nlib < MAXA) { libs[nlib] = argv[i + 1]; ++nlib; i = i + 2; }
        else if (a[0] == '-' && a[1] == 'l' && a[2] && nlib < MAXA) { libs[nlib] = a + 2; ++nlib; ++i; }
        else if (!ustrcmp(a, "-c")) { objonly = 1; ++i; }
        else if (!ustrcmp(a, "-v")) { verbose = 1; ++i; }
        else if (!ustrcmp(a, "-stack") && i + 1 < argc) {
            stack = 0; k = 0;
            while (argv[i + 1][k] >= '0' && argv[i + 1][k] <= '9') { stack = stack * 10 + argv[i + 1][k] - '0'; ++k; }
            if (!stack || argv[i + 1][k]) return usage();
            i = i + 2;
        }
        else if (!ustrcmp(a, "-P")) ++i;          // always on
        else if (a[0] == '-' && nflags < MAXA) { flags[nflags] = a; ++nflags; ++i; }
        else if (a[0] != '-' && nsrc < MAXA) { srcs[nsrc] = a; ++nsrc; ++i; }
        else return usage();
    }
    if (!nsrc) return usage();
    if (objonly && out && nsrc > 1) { ufprintf(STDERR, "cc: -c -o names one object, and there are %d sources\n", nsrc); return 2; }

    link = !objonly && (nsrc > 1 || nlib > 0);
    if (!out && !objonly) out = named(srcs[0], ".c4r");
    if (ustack(stack) < 0) { ufprintf(STDERR, "cc: -stack %d is more than a task may have\n", stack); return 2; }

    if (!link) {
        k = 0;
        while (k < nsrc) {
            if (objonly) code = compile(srcs[k], out ? out : named(srcs[k], ".c4o"), 1);
            else code = compile(srcs[k], out, 0);
            if (code) { ufprintf(STDERR, "cc: %s did not compile\n", srcs[k]); return 1; }
            ++k;
        }
        return 0;
    }

    // objects in /tmp, then c4rlink: objects, libraries, -o out
    objs = (char **)ualloc(nsrc * sizeof(char *));
    code = 0;
    k = 0;
    while (k < nsrc && !code) {
        objs[k] = cat3("/tmp/.cc-", named(srcs[k], ".c4o"), "");
        if ((code = compile(srcs[k], objs[k], 1))) ufprintf(STDERR, "cc: %s did not compile\n", srcs[k]);
        ++k;
    }
    n = k;
    if (!code) {
        av = (char **)ualloc((nsrc + 2 * nlib + 5) * sizeof(char *));
        i = 0;
        av[i] = "c4rlink"; ++i;
        k = 0;
        while (k < nsrc) { av[i] = objs[k]; ++i; ++k; }
        k = 0;
        while (k < nlib) {
            a = libs[k];
            if (ends(a, ".c4s")) { av[i] = "-l"; ++i; av[i] = a; ++i; }
            else if (ends(a, ".c4l") || ends(a, ".c4o")) { av[i] = a; ++i; }
            else { av[i] = cat3("/usr/lib/lib", a, ".c4l"); ++i; }
            ++k;
        }
        av[i] = "-o"; ++i;
        av[i] = out; ++i;
        av[i] = 0;
        if ((code = run("c4rlink", i, av))) ufprintf(STDERR, "cc: %s did not link\n", out);
    }
    k = 0;
    while (k < n) { uunlink(objs[k]); ++k; }
    return code ? 1 : 0;
}

// C4IX userland: cc. Compiles C with c4sc -- c4lc, the optimising C
// compiler, compiled to an image -- where `c4sp c4lc.lisp` runs the same
// compiler interpreted, many times slower (docs/c4sc-design.md).
//
//   cc [-O] [-c] [-conforming] [-fno-builtin] [-fpic] [-mfuse] [-v]
//      [-I dir] [-D name] [-l lib] [-o out] file.c...
//
// c4lc's own preprocessor is always on (-P), with /usr/include on the
// include path after any -I given. Without -o a program is NAME.c4r in
// the working directory, NAME the first source's file name less .c; with
// -c each source becomes its own NAME.c4o.
//
// One source and no -l is a whole program, written by c4sc as c4lc
// writes one: a program that calls only c4's own library (printf, open,
// malloc, ...) needs nothing more. More sources, or -l, are compiled to
// objects in /ram and joined by c4rlink:
//   -l c4ix          /usr/lib/libc4ix.c4l, linked in: C4IX's own calls
//   -l arc           /usr/lib/libarc.c4l (tar, gzip, crc32)
//   -l /lib/libc.c4s a shared library, loaded when the program starts
//                    (its callers are built -fno-builtin)
//
// -v prints each command before running it. c4sc recurses deeply on a
// long function, so it runs on 1 MB of stack, as the BUILD.B4K files run
// it.
#include "c4ix_user.h"

enum { MAXA = 64, CC_STACK = 1048576 };

static char *flags[MAXA];        // passed to c4sc as they are
static int   nflags;
static char *srcs[MAXA];
static int   nsrc;
static char *libs[MAXA];
static int   nlib;
static int   verbose;

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

// Runs a tool from /bin, as b4ke finds one, and answers its exit code.
static int run(char *prog, int argc, char **argv) {
    int pid, i;
    if (verbose) {
        i = 0;
        while (i < argc) { uprintf(i ? " %s" : "%s", argv[i]); ++i; }
        uprintf("\n");
    }
    pid = spawn(cat3("/bin/", prog, ""), argc, argv);
    if (pid < 0) pid = spawn(cat3(prog, ".c4r", ""), argc, argv);
    if (pid < 0) pid = spawn(cat3("c4ix-", prog, ".c4r"), argc, argv);
    if (pid < 0) { ufprintf(STDERR, "cc: cannot run %s\n", prog); return 1; }
    return uwait(pid);
}

// c4sc compile ... src out, -c for an object.
static int compile(char *src, char *out, int obj) {
    char **av;
    int n, i;
    av = (char **)ualloc((nflags + 12) * sizeof(char *));
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
    if (obj) { av[n] = "-c"; ++n; }
    av[n] = src; ++n;
    av[n] = out; ++n;
    av[n] = 0;
    return run("c4sc", n, av);
}

static int usage() {
    ufprintf(STDERR, "usage: cc [-O] [-c] [-conforming] [-fno-builtin] [-fpic] [-mfuse] [-v]\n");
    ufprintf(STDERR, "          [-I dir] [-D name] [-l lib] [-o out] file.c...\n");
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
        else if (!ustrcmp(a, "-P")) ++i;          // always on
        else if (a[0] == '-' && nflags < MAXA) { flags[nflags] = a; ++nflags; ++i; }
        else if (a[0] != '-' && nsrc < MAXA) { srcs[nsrc] = a; ++nsrc; ++i; }
        else return usage();
    }
    if (!nsrc) return usage();
    if (objonly && out && nsrc > 1) { ufprintf(STDERR, "cc: -c -o names one object, and there are %d sources\n", nsrc); return 2; }

    link = !objonly && (nsrc > 1 || nlib > 0);
    if (!out && !objonly) out = named(srcs[0], ".c4r");
    ustack(CC_STACK);

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

    // objects in /ram, then c4rlink: objects, libraries, -o out
    objs = (char **)ualloc(nsrc * sizeof(char *));
    code = 0;
    k = 0;
    while (k < nsrc && !code) {
        objs[k] = cat3("/ram/.cc-", named(srcs[k], ".c4o"), "");
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

// C4IX userland: b4ke -- the build tool, native to C4IX.
//
// C4KE's b4ke (src/c4ke/bin/b4ke.c) is the original and still builds
// C4IX under C4KE. This one reads the same files and runs on C4IX's own
// calls: spawn and wait, real directories, and files that know when
// they last changed. That last part is what B4KE2 adds: DEPS rebuilds a
// target only when one of its sources is newer, so editing one file and
// running b4ke again rebuilds what depends on it and nothing else.
//
//   b4ke [-f FILE] [-C DIR] [-n] [-k] [-B] [-j N] [NAME=VALUE ...]
//     -f FILE    build file (default BUILD.B4K)
//     -C DIR     change to DIR first; FILE is read from there
//     -n         say what would run, run nothing
//     -k         keep going after a failed step
//     -B         rebuild everything: ignore TARGET and DEPS
//     -j N       up to N steps at once, where they do not depend on each
//                other (below, "-j"); on several CPUs, at the same time
//     NAME=VALUE set NAME, overriding the file's own SET
//
// The format, one verb per line:
//
//   B4KE1 | B4KE2              magic, first line (B4KE1 files are B4KE2 files)
//   # ...                      comment
//   SET NAME word ...          $(NAME) is replaced by the words in every
//                              later line; $(DIR) is the build file's directory,
//                              and $(STACK), if set, is the stack in bytes
//                              each RUN's program gets
//   ECHO text                  progress
//   CD DIR                     later steps run in DIR
//   MKDIR DIR                  make DIR and any parents it needs
//   LIST NAME word ...         write the words to NAME, one per line, for @NAME
//   TARGET NAME                the next RUN makes NAME; skip it if NAME exists
//   DEPS NAME : SRC ...        the next RUN makes NAME from SRC...; skip it
//                              if NAME is newer than all of them
//   RUN PROG ARG ... [> FILE]  run PROG and wait; > sends its output to FILE
//   COPY SRC DST               copy a file; DST may be a directory
//   B4KE DIR [NAME=VALUE ...]  build DIR/BUILD.B4K too, with this run's flags
//
// A RUN with a TARGET or DEPS succeeds when that file is there and new
// afterwards -- a step is judged by what it made, as C4KE's b4ke judges
// it, because C4KE's compilers (which C4IX runs) do not all exit nonzero
// on an error. A RUN without one succeeds when the program exits 0.

#include "c4ix_user.h"

enum {
    B4_LINE = 4096,          // one line, after $(...) is replaced
    B4_ARGS = 256,           // words in a line
    B4_VARS = 64,
    B4_PATH = 256
};

static char *b4_file;        // build file name
static int   b4_dry;         // -n
static int   b4_keep;        // -k
static int   b4_all;         // -B
static int   b4_line;        // line number, for messages
static int   b4_ran;         // steps actually run
static int   b4_skipped;     // steps skipped as up to date
static int   b4_failed;      // steps that failed
static char *b4_target;      // pending TARGET or DEPS name, or 0
static char **b4_deps;       // pending DEPS sources
static int   b4_ndeps;
static char  b4_dir[B4_PATH];

// variables: names and values, and which came from the command line
static char *b4_vname[B4_VARS];
static char *b4_vval[B4_VARS];
static int   b4_vfixed[B4_VARS];
static int   b4_nvars;
// the command line's NAME=VALUE words, passed on to sub-builds
static char **b4_over;
static int   b4_nover;

static int b4_isspace(int c) { return c == ' ' || c == 9 || c == 13; }

static int b4_streq(char *a, char *b) {
    while (*a && *a == *b) { ++a; ++b; }
    return *a == *b;
}

// Memory, or stop: an allocation that failed and was written through
// anyway lands at address 0, under the kernel's own trap vector, and
// takes every task down with it.
static int *b4_alloc(int bytes) {
    int *p;
    if (!(p = ualloc(bytes))) {
        ufprintf(STDERR, "b4ke: out of memory\n");
        uexit(1);
    }
    return p;
}

static char *b4_dup(char *s) {
    char *r;
    int n;
    n = ustrlen(s);
    r = (char *)b4_alloc(n + 1);
    memcpy(r, s, n + 1);
    return r;
}

static char *b4_cat3(char *a, char *b, char *c) {
    char *r;
    int na, nb, nc;
    na = ustrlen(a); nb = ustrlen(b); nc = ustrlen(c);
    r = (char *)b4_alloc(na + nb + nc + 1);
    memcpy(r, a, na);
    memcpy(r + na, b, nb);
    memcpy(r + na + nb, c, nc + 1);
    return r;
}

static int b4_atoi(char *s) {
    int v;
    v = 0;
    while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); ++s; }
    return v;
}

static int b4_err(char *what, char *name) {
    ufprintf(STDERR, "b4ke: %s:%d: %s%s\n", b4_file, b4_line, what, name ? name : "");
    return 0;
}

// ---- variables ----

static int b4_find(char *name, int n) {
    int i, j;
    i = 0;
    while (i < b4_nvars) {
        j = 0;
        while (j < n && b4_vname[i][j] == name[j]) ++j;
        if (j == n && !b4_vname[i][n]) return i;
        ++i;
    }
    return 0 - 1;
}

static void b4_set(char *name, char *value, int fixed) {
    int i;
    i = b4_find(name, ustrlen(name));
    if (i >= 0) {
        if (b4_vfixed[i] && !fixed) return;      // the command line wins
    } else {
        if (b4_nvars >= B4_VARS) { b4_err("too many variables at ", name); return; }
        i = b4_nvars++;
        b4_vname[i] = b4_dup(name);
    }
    b4_vval[i] = b4_dup(value);
    b4_vfixed[i] = fixed;
}

// NAME=VALUE: set it, and return 1; anything else, 0
static int b4_assign(char *w, int fixed) {
    int i;
    char name[64];
    i = 0;
    while (w[i] && w[i] != '=' && i < 63) { name[i] = w[i]; ++i; }
    if (w[i] != '=' || i == 0) return 0;
    name[i] = 0;
    b4_set(name, w + i + 1, fixed);
    return 1;
}

// Replace every $(NAME) in SRC. An unknown name is an error.
static int b4_expand(char *src, char *out) {
    int o, n, v, k;
    char *val;
    o = 0;
    while (*src) {
        if (src[0] == '$' && src[1] == '(') {
            n = 2;
            while (src[n] && src[n] != ')') ++n;
            if (!src[n]) return b4_err("unclosed $(", 0);
            v = b4_find(src + 2, n - 2);
            if (v < 0) {
                src[n] = 0;
                return b4_err("undefined variable ", src + 2);
            }
            val = b4_vval[v];
            k = ustrlen(val);
            if (o + k >= B4_LINE) return b4_err("line too long", 0);
            memcpy(out + o, val, k);
            o = o + k;
            src = src + n + 1;
        } else {
            if (o + 1 >= B4_LINE) return b4_err("line too long", 0);
            out[o++] = *src++;
        }
    }
    out[o] = 0;
    return 1;
}

// Split a line into words in place. Returns the count; argv[n] is 0.
static int b4_words(char *p, char **argv, int max) {
    int n;
    n = 0;
    while (*p && n < max - 1) {
        while (*p && b4_isspace(*p)) ++p;
        if (!*p) break;
        argv[n++] = p;
        while (*p && !b4_isspace(*p)) ++p;
        if (*p) { *p = 0; ++p; }
    }
    argv[n] = 0;
    return n;
}

// ---- files ----

// The whole of a file, NUL-terminated, or 0.
static char *b4_slurp(char *name, int *plen) {
    int fd, n, got, cap, st[4];
    char *buf, *nb;
    if ((fd = uopen(name, O_RD)) < 0) return 0;
    cap = (ustat(name, st) == 0 && st[1] > 0) ? st[1] + 1 : 4096;
    buf = (char *)b4_alloc(cap + 1);
    got = 0;
    while (1) {
        if (got == cap) {
            nb = (char *)b4_alloc(cap * 2 + 1);
            memcpy(nb, buf, got);
            free(buf);
            buf = nb;
            cap = cap * 2;
        }
        n = uread(fd, buf + got, cap - got);
        if (n <= 0) break;
        got = got + n;
    }
    uclose(fd);
    buf[got] = 0;
    *plen = got;
    return buf;
}

static int b4_put(char *name, char *buf, int len) {
    int fd, n;
    if ((fd = uopen(name, O_WR + O_CREATE + O_TRUNCATE)) < 0) return 0;
    n = len > 0 ? write(fd, buf, len) : 0;
    uclose(fd);
    return n == len;
}

// st[3] of NAME (its place in the order of changes), or -1 if it is not there
static int b4_mtime(char *name) {
    int st[4];
    if (ustat(name, st) < 0) {
        // Not in the VFS. A relative name may still be a host file,
        // older than anything made here. An absolute one may not: the
        // board's disk answers a path it lacks with the file of the
        // same base name, so /build/bin/c4ix-ls.c4r would "exist"
        // as the disk's own c4ix-ls.c4r.
        if (name[0] == '/') return 0 - 1;
        if ((st[0] = uopen(name, O_RD)) < 0) return 0 - 1;
        uclose(st[0]);
        return 0;
    }
    return st[3];
}

static int b4_mkdirs(char *path) {
    char part[B4_PATH];
    int i, st[4];
    i = 0;
    while (path[i] && i < B4_PATH - 1) {
        part[i] = path[i];
        if (path[i] == '/' && i > 0) {
            part[i] = 0;
            if (ustat(part, st) < 0) umkdir(part);
            part[i] = '/';
        }
        ++i;
    }
    part[i] = 0;
    if (ustat(part, st) < 0) umkdir(part);
    return ustat(part, st) == 0 && st[0] == 1;
}

// ---- verbs ----

static int b4_list(char **w, int n) {
    char *buf, *p, *s;
    int i, len;
    if (b4_dry) { uprintf("b4ke: would write %s with %d name(s)\n", w[1], n - 2); return 1; }
    len = 0;
    i = 2;
    while (i < n) { len = len + ustrlen(w[i]) + 1; ++i; }
    buf = (char *)b4_alloc(len + 1);
    p = buf;
    i = 2;
    while (i < n) { s = w[i]; while (*s) *p++ = *s++; *p++ = 10; ++i; }
    if (!b4_put(w[1], buf, p - buf)) { free(buf); return b4_err("cannot write list ", w[1]); }
    free(buf);
    return 1;
}

static int b4_copy(char *src, char *dst) {
    char *buf, *base, *to;
    int len, st[4], i;
    if (b4_dry) { uprintf("b4ke: would copy %s to %s\n", src, dst); return 1; }
    if (!(buf = b4_slurp(src, &len))) return b4_err("cannot read ", src);
    to = dst;
    if (ustat(dst, st) == 0 && st[0] == 1) {
        base = src;
        i = 0;
        while (src[i]) { if (src[i] == '/') base = src + i + 1; ++i; }
        to = b4_cat3(dst, "/", base);
    }
    if (!b4_put(to, buf, len)) { free(buf); return b4_err("cannot write ", to); }
    free(buf);
    return 1;
}

// Is the pending target already good? TARGET: it exists. DEPS: it
// exists and is newer than every source that exists.
static int b4_uptodate() {
    int t, i, s;
    if (!b4_target || b4_all) return 0;
    if ((t = b4_mtime(b4_target)) < 0) return 0;
    i = 0;
    while (i < b4_ndeps) {
        s = b4_mtime(b4_deps[i]);
        if (s < 0 || s >= t) return 0;
        ++i;
    }
    return 1;
}

// The program a RUN names: /bin/NAME if there is one, else as written
// (the kernel looks in the VFS, then on the host).
static char *b4_prog(char *name) {
    char *p;
    int st[4], i;
    i = 0;
    while (name[i]) { if (name[i] == '/') return name; ++i; }
    p = b4_cat3("/bin/", name, "");
    if (ustat(p, st) == 0 && st[0] == 2) return p;
    return name;
}

static int b4_spawn(char *prog, int argc, char **argv) {
    int pid;
    char *alt;
    pid = spawn(b4_prog(prog), argc, argv);
    if (pid < 0) { alt = b4_cat3("c4ix-", prog, ".c4r"); pid = spawn(alt, argc, argv); }
    if (pid < 0) { alt = b4_cat3(prog, ".c4r", ""); pid = spawn(alt, argc, argv); }
    return pid;
}

// ---- -j: steps at once ----
//
// With -j N, a RUN is queued rather than run, and up to N run at once
// (docs/c4ix-smp.md S4: on several CPUs, at the same time). A queued step
// starts once no step before it that is still to finish makes a file it
// names -- among its DEPS sources or its words -- or names the file it
// makes; and its up-to-date test is made then, against the files as the
// steps before it left them. So the order of the file is the order of
// what depends on what, and only independent steps overlap: two compiles,
// but never a compile and the link that reads its object. A RUN with no
// TARGET or DEPS says nothing of what it makes, so it waits for every
// step before it and every step after it waits for it. The other verbs
// wait for everything queued (b4_drain): they read and write files, and
// CD would move the names the queued steps are judged by.

struct b4_job {
    char **argv;               // the RUN's words, its own copies: argv[0] is "RUN"
    int argc;
    char *out;                 // > FILE, or 0
    char *target;              // TARGET or DEPS name, or 0
    char **deps;
    int ndeps;
    int stack;                 // $(STACK) as it was when the line was read
    int line;
    int pid;                   // 0 while queued
    int before;                // the target's mtime as it started
};

enum { B4_JOBS = 512 };
static int b4_jobs = 1;      // -j N
static char *b4_jstr;        // ... as given, for sub-builds
static struct b4_job *b4_q[B4_JOBS];
static int b4_nq;            // queued and running, in the file's order
static int b4_nrun;
static int b4_stop;          // a step failed, and no -k: start nothing more

static int b4_names(struct b4_job *j, char *name) {
    int i;
    if (j->target && b4_streq(j->target, name)) return 1;
    i = 0;
    while (i < j->ndeps) { if (b4_streq(j->deps[i], name)) return 1; ++i; }
    i = 1;
    while (i < j->argc) { if (b4_streq(j->argv[i], name)) return 1; ++i; }
    return 0;
}

// May queue entry k start? Nothing before it still to finish may make
// what it uses, or use what it makes; a step that says nothing waits for,
// and holds up, everything.
static int b4_ready(int k) {
    struct b4_job *j, *e;
    int i;
    j = b4_q[k];
    i = 0;
    while (i < k) {
        e = b4_q[i];
        if (!e->target || !j->target) return 0;
        if (b4_names(j, e->target) || b4_names(e, j->target)) return 0;
        if (e->out && b4_names(j, e->out)) return 0;
        ++i;
    }
    return 1;
}

static void b4_free_job(struct b4_job *j) {
    int i;
    i = 0;
    while (i < j->argc) { free(j->argv[i]); ++i; }
    free((char *)j->argv);
    free((char *)j);
}

static void b4_dequeue(int k) {
    while (k + 1 < b4_nq) { b4_q[k] = b4_q[k + 1]; ++k; }
    --b4_nq;
}

// Start queue entry k: or skip it, up to date
static void b4_start(int k) {
    struct b4_job *j;
    int fd, saved, i;
    j = b4_q[k];
    b4_target = j->target; b4_deps = j->deps; b4_ndeps = j->ndeps;
    i = b4_uptodate();
    b4_target = 0; b4_ndeps = 0;
    if (i) { ++b4_skipped; b4_dequeue(k); b4_free_job(j); return; }
    j->before = j->target ? b4_mtime(j->target) : 0;
    uprintf("+");
    i = 1;
    while (i < j->argc) uprintf(" %s", j->argv[i++]);
    if (j->out) uprintf(" > %s", j->out);
    uprintf("\n");
    fd = 0 - 1; saved = 0 - 1;
    if (j->out) {
        if ((fd = uopen(j->out, O_WR + O_CREATE + O_TRUNCATE)) < 0) {
            ++b4_failed; b4_line = j->line; b4_err("cannot write ", j->out);
            if (!b4_keep) b4_stop = 1;
            b4_dequeue(k); b4_free_job(j);
            return;
        }
        saved = udup(STDOUT);
        udup2(fd, STDOUT);
    }
    ustack(j->stack);
    j->pid = b4_spawn(j->argv[1], j->argc - 1, j->argv + 1);
    if (j->out) { udup2(saved, STDOUT); uclose(saved); uclose(fd); }
    if (j->pid < 0) {
        ++b4_failed; b4_line = j->line; b4_err("cannot start ", j->argv[1]);
        if (!b4_keep) b4_stop = 1;
        b4_dequeue(k); b4_free_job(j);
        return;
    }
    ++b4_nrun;
}

// A running step ended: judge it as b4_run does
static void b4_reap() {
    struct b4_job *j;
    int st[2], pid, k, after;
    pid = uwaitpid(0 - 1, st, 0);
    k = 0;
    while (k < b4_nq && b4_q[k]->pid != pid) ++k;
    if (k == b4_nq) { if (pid < 0) b4_nrun = 0; return; }   // not ours
    j = b4_q[k];
    --b4_nrun;
    ++b4_ran;
    b4_line = j->line;
    if (j->target) {
        after = b4_mtime(j->target);
        if (after < 0 || after <= j->before) {
            ++b4_failed;
            ufprintf(STDERR, "b4ke: %s:%d: %s did not make %s\n", b4_file, j->line, j->argv[1], j->target);
            if (!b4_keep) b4_stop = 1;
        }
    } else if (st[0] != 0) {
        ++b4_failed;
        ufprintf(STDERR, "b4ke: %s:%d: %s exited %d\n", b4_file, j->line, j->argv[1], st[0]);
        if (!b4_keep) b4_stop = 1;
    }
    b4_dequeue(k);
    b4_free_job(j);
}

// Start what may start; then, with ALL, wait until nothing is queued or
// running, and without it until there is room for another step.
static void b4_pump(int all) {
    int k, started;
    for (;;) {
        started = 1;
        while (started && !b4_stop && b4_nrun < b4_jobs) {
            started = 0;
            k = 0;
            while (k < b4_nq && b4_nrun < b4_jobs) {
                if (!b4_q[k]->pid && b4_ready(k)) { b4_start(k); started = 1; break; }
                ++k;
            }
        }
        if (b4_stop) {
            // a failure: what has not started never will
            k = 0;
            while (k < b4_nq) { if (!b4_q[k]->pid) { b4_free_job(b4_q[k]); b4_dequeue(k); } else ++k; }
        }
        if (!b4_nq) return;
        if (!all && b4_nrun < b4_jobs && b4_nq < B4_JOBS) return;
        if (!b4_nrun) return;     // cannot happen: the first entry is always ready
        b4_reap();
    }
}

static void b4_drain() {
    if (b4_jobs > 1) b4_pump(1);
}

// RUN, with -j: queued, with its own copies of everything it needs
static int b4_queue(char **w, int n) {
    struct b4_job *j;
    int i;
    j = (struct b4_job *)b4_alloc(sizeof(struct b4_job));
    j->out = 0;
    if (n >= 4 && b4_streq(w[n - 2], ">")) { j->out = b4_dup(w[n - 1]); n = n - 2; }
    j->argv = (char **)b4_alloc((n + 1) * sizeof(int));
    i = 0;
    while (i < n) { j->argv[i] = b4_dup(w[i]); ++i; }
    j->argv[n] = 0;
    j->argc = n;
    j->target = b4_target;
    j->deps = b4_deps;
    j->ndeps = b4_ndeps;
    b4_target = 0; b4_ndeps = 0;
    i = b4_find("STACK", 5);
    j->stack = i >= 0 ? b4_atoi(b4_vval[i]) : 0;
    j->line = b4_line;
    j->pid = 0;
    b4_q[b4_nq++] = j;
    b4_pump(0);
    return !b4_stop;
}

static int b4_run(char **w, int n) {
    int pid, code, before, after, fd, saved, i;
    char *out;

    if (b4_jobs > 1 && !b4_dry) return b4_queue(w, n);
    if (b4_uptodate()) {
        ++b4_skipped;
        b4_target = 0; b4_ndeps = 0;
        return 1;
    }
    // "> FILE" at the end: the program's fd 1
    out = 0;
    if (n >= 4 && b4_streq(w[n - 2], ">")) { out = w[n - 1]; n = n - 2; w[n] = 0; }
    if (b4_dry) {
        uprintf("b4ke: would run:");
        i = 1;
        while (i < n) uprintf(" %s", w[i++]);
        if (out) uprintf(" > %s", out);
        uprintf("\n");
        b4_target = 0; b4_ndeps = 0;
        return 1;
    }
    before = b4_target ? b4_mtime(b4_target) : 0;
    // say what runs, as make does -- the Editor reads this to know which
    // source an error that names only a line belongs to
    uprintf("+");
    i = 1;
    while (i < n) uprintf(" %s", w[i++]);
    if (out) uprintf(" > %s", out);
    uprintf("\n");

    fd = 0 - 1; saved = 0 - 1;
    if (out) {
        if ((fd = uopen(out, O_WR + O_CREATE + O_TRUNCATE)) < 0) return b4_err("cannot write ", out);
        saved = udup(STDOUT);
        udup2(fd, STDOUT);
    }
    // $(STACK), when set, is the stack in bytes for what this runs:
    // c4sc compiling a large module needs more than a task's default
    i = b4_find("STACK", 5);
    ustack(i >= 0 ? b4_atoi(b4_vval[i]) : 0);
    // w[1] is the program; it is argv[0] of the child
    pid = b4_spawn(w[1], n - 1, w + 1);
    if (out) { udup2(saved, STDOUT); uclose(saved); uclose(fd); }
    if (pid < 0) { ++b4_failed; b4_target = 0; b4_ndeps = 0; return b4_err("cannot start ", w[1]); }
    code = uwait(pid);
    ++b4_ran;

    if (b4_target) {
        after = b4_mtime(b4_target);
        if (after < 0 || after <= before) {
            ++b4_failed;
            ufprintf(STDERR, "b4ke: %s:%d: %s did not make %s\n", b4_file, b4_line, w[1], b4_target);
            b4_target = 0; b4_ndeps = 0;
            return 0;
        }
    } else if (code != 0) {
        ++b4_failed;
        ufprintf(STDERR, "b4ke: %s:%d: %s exited %d\n", b4_file, b4_line, w[1], code);
        return 0;
    }
    b4_target = 0; b4_ndeps = 0;
    return 1;
}

// B4KE DIR [NAME=VALUE ...]: a sub-build, as its own task
static int b4_sub(char **w, int n) {
    char **av;
    int ac, i, pid, code;
    av = (char **)b4_alloc((n + b4_nover + 10) * sizeof(int));
    ac = 0;
    av[ac++] = "b4ke";
    av[ac++] = "-C";
    av[ac++] = w[1];
    if (b4_jobs > 1) { av[ac++] = "-j"; av[ac++] = b4_jstr; }
    if (b4_dry) av[ac++] = "-n";
    if (b4_keep) av[ac++] = "-k";
    if (b4_all) av[ac++] = "-B";
    i = 0;
    while (i < b4_nover) av[ac++] = b4_over[i++];
    i = 2;
    while (i < n) av[ac++] = w[i++];
    av[ac] = 0;
    if ((pid = b4_spawn("b4ke", ac, av)) < 0) return b4_err("cannot start b4ke for ", w[1]);
    code = uwait(pid);
    if (code != 0) { ++b4_failed; return b4_err("sub-build failed: ", w[1]); }
    return 1;
}

static int b4_step(char **w, int n) {
    int i;
    // what reads or writes files, or moves where names are, waits for
    // the steps queued before it (-j)
    if (b4_streq(w[0], "CD") || b4_streq(w[0], "MKDIR") || b4_streq(w[0], "LIST") ||
        b4_streq(w[0], "COPY") || b4_streq(w[0], "B4KE")) {
        b4_drain();
        if (b4_stop) return 0;
    }
    if (b4_streq(w[0], "ECHO")) {
        i = 1;
        while (i < n) { uprintf("%s", w[i]); ++i; if (i < n) uprintf(" "); }
        uprintf("\n");
        return 1;
    }
    if (b4_streq(w[0], "SET")) {
        char *val, *p;
        int len;
        if (n < 2) return b4_err("SET needs a name", 0);
        len = 0;
        i = 2;
        while (i < n) { len = len + ustrlen(w[i]) + 1; ++i; }
        val = (char *)b4_alloc(len + 1);
        p = val;
        i = 2;
        while (i < n) {
            memcpy(p, w[i], ustrlen(w[i]));
            p = p + ustrlen(w[i]);
            ++i;
            if (i < n) *p++ = ' ';
        }
        *p = 0;
        b4_set(w[1], val, 0);
        return 1;
    }
    if (b4_streq(w[0], "CD")) {
        if (n < 2) return b4_err("CD needs a directory", 0);
        if (uchdir(w[1]) < 0) return b4_err("no such directory: ", w[1]);
        return 1;
    }
    if (b4_streq(w[0], "MKDIR")) {
        if (n < 2) return b4_err("MKDIR needs a directory", 0);
        if (b4_dry) return 1;
        if (!b4_mkdirs(w[1])) return b4_err("cannot make ", w[1]);
        return 1;
    }
    if (b4_streq(w[0], "TARGET")) {
        if (n < 2) return b4_err("TARGET needs a name", 0);
        b4_target = b4_dup(w[1]);
        b4_ndeps = 0;
        return 1;
    }
    if (b4_streq(w[0], "DEPS")) {
        if (n < 3 || !b4_streq(w[2], ":")) return b4_err("DEPS wants NAME : SOURCES", 0);
        b4_target = b4_dup(w[1]);
        b4_deps = (char **)b4_alloc((n + 1) * sizeof(int));
        b4_ndeps = 0;
        i = 3;
        while (i < n) b4_deps[b4_ndeps++] = b4_dup(w[i++]);
        return 1;
    }
    if (b4_streq(w[0], "LIST")) {
        if (n < 2) return b4_err("LIST needs a name", 0);
        if (!b4_list(w, n)) { ++b4_failed; return 0; }
        return 1;
    }
    if (b4_streq(w[0], "RUN")) {
        if (n < 2) return b4_err("RUN needs a program", 0);
        return b4_run(w, n);
    }
    if (b4_streq(w[0], "COPY")) {
        if (n < 3) return b4_err("COPY needs a source and a destination", 0);
        if (!b4_copy(w[1], w[2])) { ++b4_failed; return 0; }
        return 1;
    }
    if (b4_streq(w[0], "B4KE")) {
        if (n < 2) return b4_err("B4KE needs a directory", 0);
        return b4_sub(w, n);
    }
    b4_err("unknown verb ", w[0]);
    ++b4_failed;
    return 0;
}

static int usage() {
    ufprintf(STDERR, "usage: b4ke [-f FILE] [-C DIR] [-n] [-k] [-B] [-j N] [NAME=VALUE ...]\n");
    return 2;
}

int main(int argc, char **argv) {
    char *text, *p, *eol, *dir, *raw, *q;
    char *line;
    char **words;
    int len, n, i, ok;

    b4_file = "BUILD.B4K";
    dir = 0;
    b4_over = (char **)b4_alloc((argc + 1) * sizeof(int));
    b4_nover = 0;
    i = 1;
    while (i < argc) {
        if (b4_streq(argv[i], "-f") && i + 1 < argc) { ++i; b4_file = argv[i]; }
        else if (b4_streq(argv[i], "-C") && i + 1 < argc) { ++i; dir = argv[i]; }
        else if (b4_streq(argv[i], "-n")) b4_dry = 1;
        else if (b4_streq(argv[i], "-k")) b4_keep = 1;
        else if (b4_streq(argv[i], "-B")) b4_all = 1;
        else if (b4_streq(argv[i], "-j") && i + 1 < argc) {
            ++i;
            b4_jstr = argv[i];
            b4_jobs = b4_atoi(argv[i]);
            if (b4_jobs < 1) b4_jobs = 1;
        }
        else if (b4_assign(argv[i], 1)) b4_over[b4_nover++] = argv[i];
        else return usage();
        ++i;
    }
    if (dir && uchdir(dir) < 0) { ufprintf(STDERR, "b4ke: no such directory: %s\n", dir); return 1; }
    if (ugetcwd(b4_dir, B4_PATH) < 0) { b4_dir[0] = '/'; b4_dir[1] = 0; }
    b4_set("DIR", b4_dir, 1);

    if (!(text = b4_slurp(b4_file, &len))) {
        ufprintf(STDERR, "b4ke: cannot read %s in %s\n", b4_file, b4_dir);
        return 1;
    }
    line = (char *)b4_alloc(B4_LINE);
    words = (char **)b4_alloc(B4_ARGS * sizeof(int));

    p = text;
    ok = 1;
    b4_line = 0;
    while (*p && (ok || b4_keep)) {
        eol = p;
        while (*eol && *eol != 10) ++eol;
        if (*eol) { *eol = 0; ++eol; }
        ++b4_line;
        raw = p;
        p = eol;
        q = raw;
        while (*q && b4_isspace(*q)) ++q;
        if (!*q || *q == '#') continue;
        if (b4_line == 1) {
            n = b4_words(raw, words, B4_ARGS);
            if (!b4_streq(words[0], "B4KE1") && !b4_streq(words[0], "B4KE2")) {
                ufprintf(STDERR, "b4ke: %s does not begin with B4KE1 or B4KE2\n", b4_file);
                return 1;
            }
            continue;
        }
        // $(...) is replaced in the line before it is split, so a
        // variable may stand for several words
        if (!b4_expand(raw, line)) { ++b4_failed; ok = 0; continue; }
        n = b4_words(line, words, B4_ARGS);
        if (!n) continue;
        if (!b4_step(words, n)) ok = 0;
    }
    b4_drain();

    uprintf("b4ke: %s: %d ran, %d up to date, %d failed\n", b4_dir, b4_ran, b4_skipped, b4_failed);
    return b4_failed ? 1 : 0;
}

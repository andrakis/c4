//
// c4ix-sh: the C4IX shell.
//
// An ordinary userland program -- protected mode and all -- built
// from the syscalls X2 and X3 provide. It reads commands from a
// script (argv[1]) or from fd 0, and supports what a shell needs to
// be a shell: pipelines, input and output redirection, background
// jobs, and a few builtins.
//
// There is no fork here. A stage is run by pointing the SHELL's own
// fd 0 and fd 1 wherever that stage needs them, spawning (the child
// inherits the descriptor table), and then putting the shell's own
// descriptors back. That is the whole trick, and it is why X3's
// dup2-shares-a-description semantics had to be right.
//
//   cd / pwd / ls   -- the RAM filesystem has directories, so these
//                      are real: cd and pwd are builtins (they change
//                      the shell's own state), ls is a program.
//   cmd < in > out  -- redirection
//   a | b | c       -- pipelines of any length
//   cmd &           -- background, reported by `jobs`, reaped by `wait`
//   builtins        -- exit, jobs, wait, cd, help
//

#include "c4ix_user.h"

enum { LINE_MAX = 256, WORD_MAX = 32, ARG_MAX = 16, JOB_MAX = 8 };
enum { RDBUF = 512 };

// ---- buffered line input ----
//
// One read syscall per bufferful, not per character: through the
// trap gateway a syscall is a kernel round trip.
static char rdbuf[RDBUF];
static int  rdlen;
static int  rdpos;
static int  script_fd;

// Returns the line length, or -1 at end of input.
static int sh_getline(char *line, int max) {
    int n, c, got;

    n = 0;
    got = 0;
    while (1) {
        if (rdpos >= rdlen) {
            rdlen = uread(script_fd, rdbuf, RDBUF);
            rdpos = 0;
            if (rdlen <= 0) { line[n] = 0; return got ? n : -1; }
        }
        c = rdbuf[rdpos]; ++rdpos;
        got = 1;
        if (c == '\n') { line[n] = 0; return n; }
        if (n < max - 1) { line[n] = c; ++n; }
    }
}

// ---- tokenizing ----
//
// Words split on whitespace; the operators | < > & are single-
// character words whether or not they are spaced, so "a|b" and
// "a | b" both work. '#' starts a comment. Operators are handed back
// as pointers to these constants so the line itself is only ever
// split, never rewritten around them.
static char *tok_pipe = "|";
static char *tok_in   = "<";
static char *tok_out  = ">";
static char *tok_amp  = "&";

static int sh_special(int c) {
    if (c == '|') return 1;
    if (c == '<') return 1;
    if (c == '>') return 1;
    if (c == '&') return 1;
    return 0;
}

static char *sh_optoken(int c) {
    if (c == '|') return tok_pipe;
    if (c == '<') return tok_in;
    if (c == '>') return tok_out;
    return tok_amp;
}

static int sh_split(char *line, char **words, int max) {
    char *p;
    int n, c;

    n = 0;
    p = line;
    while (*p && n < max) {
        while (*p == ' ' || *p == 9) ++p;
        if (!*p) break;
        if (*p == '#') break;
        if (sh_special(*p)) {
            words[n] = sh_optoken(*p); ++n;
            ++p;
            continue;
        }
        words[n] = p; ++n;
        while (*p && *p != ' ' && *p != 9 && !sh_special(*p)) ++p;
        if (!*p) break;
        // Terminate the word in place. If an operator ended it, the
        // character is about to be overwritten -- so emit its token
        // now, from the constants above, and nothing is lost.
        c = *p;
        *p = 0;
        ++p;
        if (sh_special(c) && n < max) { words[n] = sh_optoken(c); ++n; }
    }
    return n;
}

// ---- command resolution ----
//
// A bare name becomes c4ix-<name>.c4r; anything containing a dot is
// taken as a path. Programs live on the host filesystem, but the
// kernel checks its RAM files first, so a generated program would be
// found the same way.
static char resolved[64];

// append src at dst, returning the position of the new terminator
static char *sh_cat(char *dst, char *src) {
    while (*src) { *dst = *src; ++dst; ++src; }
    *dst = 0;
    return dst;
}

static char *sh_resolve(char *name) {
    char *p;
    int i;

    i = 0;
    while (name[i]) {
        if (name[i] == '.' || name[i] == '/') return name;
        ++i;
    }
    p = sh_cat(resolved, "c4ix-");
    p = sh_cat(p, name);
    sh_cat(p, ".c4r");
    return resolved;
}

static int sh_atoi(char *s) {
    int n, neg;
    n = 0;
    neg = 0;
    if (*s == '-') { neg = 1; ++s; }
    while (*s >= '0' && *s <= '9') { n = n * 10 + (*s - '0'); ++s; }
    return neg ? -n : n;
}

// Children read their argv out of the parent's memory, so a
// backgrounded command must not have its argv reused by the next
// line. Every spawn therefore gets its own copy.
static char **sh_dup_argv(char **argv, int argc) {
    char **out;
    char *blob;
    int i, bytes;

    bytes = 0;
    i = 0;
    while (i < argc) { bytes = bytes + ustrlen(argv[i]) + 1; ++i; }
    if (!(out = (char **)ualloc(sizeof(char *) * (argc + 1) + bytes))) return 0;
    blob = (char *)(out + argc + 1);
    i = 0;
    while (i < argc) {
        out[i] = blob;
        blob = ustrcpy(blob, argv[i]);
        ++i;
    }
    out[argc] = 0;
    return out;
}

// ---- jobs ----

static int  job_pid[JOB_MAX];
static char *job_name[JOB_MAX];
static int  njobs;

static void sh_addjob(int pid, char *name) {
    if (njobs >= JOB_MAX) { uprintf("sh: too many jobs\n"); return; }
    job_pid[njobs] = pid;
    job_name[njobs] = name;
    ++njobs;
    uprintf("[%d] started %d %s\n", njobs, pid, name);
}

static void sh_jobs() {
    int i;
    if (!njobs) { uprintf("sh: no background jobs\n"); return; }
    i = 0;
    while (i < njobs) {
        uprintf("[%d] running %d %s\n", i + 1, job_pid[i], job_name[i]);
        ++i;
    }
}

static void sh_waitall() {
    int i, st;
    i = 0;
    while (i < njobs) {
        st = uwait(job_pid[i]);
        uprintf("[%d] %s done (%d)\n", i + 1, job_name[i], st);
        ++i;
    }
    njobs = 0;
}

// ---- builtins ----
//
// Returns 1 if the word was a builtin and has been handled.
static int sh_exiting;
static int sh_status;

static int sh_builtin(char **argv, int argc) {
    if (!ustrcmp(argv[0], "exit")) {
        sh_exiting = 1;
        if (argc > 1) sh_status = sh_atoi(argv[1]);
        return 1;
    }
    if (!ustrcmp(argv[0], "jobs")) { sh_jobs(); return 1; }
    if (!ustrcmp(argv[0], "wait")) { sh_waitall(); return 1; }
    if (!ustrcmp(argv[0], "help")) {
        uprintf("c4ix-sh: cmd [args] [< in] [> out] [| cmd ...] [&]\n");
        uprintf("builtins: exit jobs wait cd pwd help\n");
        return 1;
    }
    if (!ustrcmp(argv[0], "cd")) {
        char *dir;
        dir = (argc > 1) ? argv[1] : "/";
        if (uchdir(dir) < 0) uprintf("cd: no such directory: %s\n", dir);
        return 1;
    }
    if (!ustrcmp(argv[0], "pwd")) {
        char cwd[128];
        if (ugetcwd(cwd, 128) > 0) uprintf("%s\n", cwd);
        else uprintf("pwd: failed\n");
        return 1;
    }
    return 0;
}

// ---- running a line ----

static int sh_runline(char **w, int nw) {
    char *argv[ARG_MAX];
    char **spawned;
    char *infile, *outfile;
    int i, argc, background, hasnext, pid, fd, st;
    int prev_read, saved0, saved1;
    int p[2];
    int pids[ARG_MAX];
    int npids;

    background = 0;
    if (nw > 0) {
        if (w[nw - 1] == tok_amp) { background = 1; --nw; }
    }
    if (!nw) return 0;

    prev_read = -1;
    npids = 0;
    st = 0;
    i = 0;
    while (i < nw) {
        argc = 0;
        infile = 0;
        outfile = 0;
        while (i < nw && w[i] != tok_pipe) {
            if (w[i] == tok_in) { ++i; if (i < nw) infile = w[i]; }
            else if (w[i] == tok_out) { ++i; if (i < nw) outfile = w[i]; }
            else if (argc < ARG_MAX - 1) { argv[argc] = w[i]; ++argc; }
            ++i;
        }
        argv[argc] = 0;
        hasnext = 0;
        if (i < nw) { if (w[i] == tok_pipe) { hasnext = 1; ++i; } }
        if (!argc) { uprintf("sh: empty command\n"); return 1; }

        // A builtin only makes sense unpiped and unredirected: it
        // runs in the shell itself, which has nowhere to put a pipe.
        if (!hasnext && prev_read < 0 && !infile && !outfile) {
            if (sh_builtin(argv, argc)) return 0;
        }

        if (hasnext) {
            if (upipe(p) < 0) { uprintf("sh: pipe failed\n"); return 1; }
        }

        // Point our own descriptors at this stage's ends, spawn, and
        // put them back. The child inherits the table as it stands.
        saved0 = udup(STDIN);
        saved1 = udup(STDOUT);
        if (prev_read >= 0) udup2(prev_read, STDIN);
        else if (infile) {
            if ((fd = uopen(infile, O_RD)) < 0) uprintf("sh: cannot open %s\n", infile);
            else { udup2(fd, STDIN); uclose(fd); }
        }
        if (hasnext) udup2(p[1], STDOUT);
        else if (outfile) {
            if ((fd = uopen(outfile, O_WR + O_CREATE + O_TRUNCATE)) < 0)
                uprintf("sh: cannot create %s\n", outfile);
            else { udup2(fd, STDOUT); uclose(fd); }
        }

        spawned = sh_dup_argv(argv, argc);
        pid = spawn(sh_resolve(argv[0]), argc, spawned);

        udup2(saved0, STDIN);  uclose(saved0);
        udup2(saved1, STDOUT); uclose(saved1);
        if (prev_read >= 0) uclose(prev_read);
        if (hasnext) { uclose(p[1]); prev_read = p[0]; }
        else prev_read = -1;

        if (pid < 0) { uprintf("sh: cannot run %s\n", argv[0]); return 1; }
        if (background) sh_addjob(pid, spawned[0]);
        else { pids[npids] = pid; ++npids; }
    }

    // Foreground: wait for every stage; the last one gives the status.
    i = 0;
    while (i < npids) { st = uwait(pids[i]); ++i; }
    return st;
}

int main(int argc, char **argv) {
    char line[LINE_MAX];
    char *words[WORD_MAX];
    int nw, len;

    script_fd = STDIN;
    if (argc > 1) {
        if ((script_fd = uopen(argv[1], O_RD)) < 0) {
            ufprintf(STDERR, "sh: cannot open %s\n", argv[1]);
            return 1;
        }
    }

    while (!sh_exiting) {
        if ((len = sh_getline(line, LINE_MAX)) < 0) break;
        if (!len) continue;
        nw = sh_split(line, words, WORD_MAX);
        if (!nw) continue;
        sh_runline(words, nw);
    }

    if (njobs) sh_waitall();
    if (script_fd != STDIN) uclose(script_fd);
    return sh_status;
}

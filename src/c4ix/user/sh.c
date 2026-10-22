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
//   *.c, a?.txt     -- wildcards in a word's last component (sh_glob)
//   builtins        -- exit, jobs, wait, cd, pwd, rm [-r] [-f], mv, help
//

#include "c4ix_user.h"

// ARG_MAX is an expanded line's words: rm /build/bin/* is a few dozen
enum { LINE_MAX = 256, WORD_MAX = 32, ARG_MAX = 512, JOB_MAX = 8 };
enum { RDBUF = 512, GLOB_POOL = 32768, NAME_MAX = 64, PATHW = 256 };

// ---- buffered line input ----
//
// One read syscall per bufferful, not per character: through the
// trap gateway a syscall is a kernel round trip.
static char rdbuf[RDBUF];
static int  rdlen;
static int  rdpos;
static int  script_fd;
static int  interactive;   // reading fd 0 with no script: prompt
static int  inline_prompt; // -p: the prompt stays on the input line

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

// ---- wildcards ----
//
// A word with * (any run of characters) or ? (any one) in its last
// component becomes the names in that directory it matches, sorted, as
// sh does it: rm /build/bin/*, cat *.txt. A name starting with '.' is
// matched only by a pattern that does too. No match leaves the word as
// written, so a command can say what it could not find. The directory
// part is taken as it is: /usr/*/x is not expanded. The names live in a
// pool kept for the line; a background job copies its own (sh_dup_argv).
static char *glob_pool;
static int glob_used;

static int sh_wild(char *w) {
    while (*w) { if (*w == '*' || *w == '?') return 1; ++w; }
    return 0;
}

static int sh_match(char *p, char *s) {
    if (!*p) return !*s;
    if (*p == '*') {
        while (1) {
            if (sh_match(p + 1, s)) return 1;
            if (!*s) return 0;
            ++s;
        }
    }
    if (!*s) return 0;
    if (*p == '?' || *p == *s) return sh_match(p + 1, s + 1);
    return 0;
}

// Expand WORD into argv from K, up to MAX. Returns how many it added.
static int sh_glob(char *word, char **argv, int k, int max) {
    char dir[PATHW], name[NAME_MAX];
    char *pat, *s, *t;
    int i, slash, start, need, j;

    slash = 0 - 1;
    i = 0;
    while (word[i]) { if (word[i] == '/') slash = i; ++i; }
    if (slash >= PATHW - 1) return 0;
    pat = word + slash + 1;
    if (slash < 0) { dir[0] = '.'; dir[1] = 0; }
    else if (slash == 0) { dir[0] = '/'; dir[1] = 0; }
    else { i = 0; while (i < slash) { dir[i] = word[i]; ++i; } dir[i] = 0; }
    if (sh_wild(dir)) return 0;
    if (!glob_pool && !(glob_pool = (char *)ualloc(GLOB_POOL))) return 0;
    start = k;
    i = 0;
    while (k < max && ureaddir(dir, i, name) >= 0) {
        ++i;
        if (name[0] == '.' && pat[0] != '.') continue;
        if (!sh_match(pat, name)) continue;
        need = slash + 1 + ustrlen(name) + 1;
        if (glob_used + need > GLOB_POOL) { uprintf("sh: too many names for %s\n", word); break; }
        s = glob_pool + glob_used;
        j = 0;
        while (j <= slash) { s[j] = word[j]; ++j; }
        ustrcpy(s + j, name);
        glob_used = glob_used + need;
        argv[k] = s;
        ++k;
    }
    // sorted, as the names would be listed
    i = start + 1;
    while (i < k) {
        j = i;
        while (j > start && ustrcmp(argv[j], argv[j - 1]) < 0) {
            t = argv[j]; argv[j] = argv[j - 1]; argv[j - 1] = t;
            --j;
        }
        ++i;
    }
    return k - start;
}

// ---- rm ----
//
// rm [-r] [-f] PATH...: -r takes a directory and everything in it, files
// first and then the directories as they empty; -f says nothing of what
// it could not remove. Without -r an empty directory still goes, as the
// kernel's unlink allows.

// DIR's contents, then DIR. How many could not be removed.
static int sh_rmtree(char *dir, int quiet) {
    char name[NAME_MAX], sub[PATHW];
    int i, r, bad, n;
    bad = 0;
    i = 0;
    while ((r = ureaddir(dir, i, name)) >= 0) {
        n = ustrlen(dir);
        if (n + ustrlen(name) + 2 > PATHW) { ++bad; ++i; continue; }
        ustrcpy(sub, dir);
        if (!(n == 1 && dir[0] == '/')) { sub[n] = '/'; sub[n + 1] = 0; }
        ustrcpy(sub + ustrlen(sub), name);
        if (r == 1) n = sh_rmtree(sub, quiet);
        else {
            n = uunlink(sub) < 0;
            if (n && !quiet) uprintf("rm: cannot remove %s\n", sub);
        }
        // what went no longer takes index i; what stayed does
        if (n) { bad = bad + n; ++i; }
    }
    if (uunlink(dir) < 0) {
        if (!bad && !quiet) uprintf("rm: cannot remove %s\n", dir);
        ++bad;
    }
    return bad;
}

static void sh_rm(char **argv, int argc) {
    int k, j, rec, quiet, st[4];
    rec = 0; quiet = 0;
    k = 1;
    while (k < argc && argv[k][0] == '-' && argv[k][1]) {
        j = 1;
        while (argv[k][j]) {
            if (argv[k][j] == 'r' || argv[k][j] == 'R') rec = 1;
            else if (argv[k][j] == 'f') quiet = 1;
            else { uprintf("usage: rm [-r] [-f] PATH...\n"); return; }
            ++j;
        }
        ++k;
    }
    if (k == argc && !quiet) uprintf("usage: rm [-r] [-f] PATH...\n");
    while (k < argc) {
        if (rec && ustat(argv[k], st) == 0 && st[0] == 1) sh_rmtree(argv[k], quiet);
        else if (uunlink(argv[k]) < 0 && !quiet) {
            if (ustat(argv[k], st) == 0 && st[0] == 1) uprintf("rm: %s is a directory with things in it: rm -r\n", argv[k]);
            else uprintf("rm: cannot remove %s\n", argv[k]);
        }
        ++k;
    }
}

// ---- command resolution ----
//
// A bare name is tried as /bin/NAME first, then c4ix-NAME.c4r, then as
// written, then with .c4r appended. The order matters in both directions: C4IX's
// own programs win, because this tree also contains C4KE binaries
// with the same names (ps.c4r, cat.c4r, echo.c4r) which are built
// against a different kernel's ABI and die on an illegal opcode
// here; and anything else -- innerbench.c4r, c4m.c4r, whatever c4cc
// produced -- still runs by name. A name containing '/' or '.' is a
// path and is used exactly as written. Programs live on the host
// filesystem, but the kernel checks its RAM files first, so a
// generated program is found the same way.
static char resolved[80];

// append src at dst, returning the position of the new terminator
static char *sh_cat(char *dst, char *src) {
    while (*src) { *dst = *src; ++dst; ++src; }
    *dst = 0;
    return dst;
}

// Build candidate FORM for NAME: 0 /bin/NAME, 1 with the c4ix- prefix
// and .c4r, 2 as written, 3 with .c4r. Returns 0 when there are no
// more. /bin comes first so that a program rebuilt inside the machine
// and installed there (b4ke -f INSTALL.B4K) is the one that runs; until
// then /bin holds the same images the host names do.
static char *sh_candidate(char *name, int form) {
    char *p;
    int i, hasdot, st[4];

    hasdot = 0;
    i = 0;
    while (name[i]) {
        if (name[i] == '.') hasdot = 1;
        if (name[i] == '/') hasdot = 1;
        ++i;
    }
    if (hasdot) return (form == 0) ? name : 0;   // a path stands for itself
    if (form == 0) {
        // only C4IX's own /bin: on a native host, an absent /bin/NAME
        // would fall through to the HOST's /bin (/bin/echo is an ELF)
        p = sh_cat(resolved, "/bin/");
        sh_cat(p, name);
        if (ustat(resolved, st) < 0 || st[0] != 2) return 0;
        return resolved;
    }
    if (form == 1) {
        p = sh_cat(resolved, "c4ix-");
        p = sh_cat(p, name);
        sh_cat(p, ".c4r");
        return resolved;
    }
    if (form == 2) return name;
    if (form == 3) {
        p = sh_cat(resolved, name);
        sh_cat(p, ".c4r");
        return resolved;
    }
    return 0;
}

static int sh_atoi(char *s) {
    int n, neg;
    n = 0;
    neg = 0;
    if (*s == '-') { neg = 1; ++s; }
    while (*s >= '0' && *s <= '9') { n = n * 10 + (*s - '0'); ++s; }
    return neg ? -n : n;
}

// A copy of one command's argv, out of the line buffer the next line
// reuses. The child gets the kernel's own copy at spawn (sys.c), so only
// a background job keeps this one, as its name in the job list; the rest
// are freed once spawned, or the shell would keep every line it ran.
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
static char **job_argv[JOB_MAX];   // sh_dup_argv's block, freed with the job
static int  njobs;

static void sh_addjob(int pid, char **av) {
    if (njobs >= JOB_MAX) { uprintf("sh: too many jobs\n"); free(av); return; }
    job_pid[njobs] = pid;
    job_name[njobs] = av[0];
    job_argv[njobs] = av;
    ++njobs;
    uprintf("[%d] started %d %s\n", njobs, pid, av[0]);
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

// kill [-SIG] %JOB|PID ...
//
// A job number is what the person has in front of them, since `jobs`
// printed it, so "%1" is the natural handle; a bare number is a pid,
// for anything this shell did not start. The default is 15 (TERM),
// which for a program with no handler simply cancels it.
static void sh_kill(char **argv, int argc) {
    int i, sig, pid, n;

    sig = 15;
    i = 1;
    if (i < argc && argv[i][0] == '-') { sig = sh_atoi(argv[i] + 1); ++i; }
    if (i >= argc) { uprintf("kill: usage: kill [-SIG] %%JOB | PID\n"); return; }
    while (i < argc) {
        if (argv[i][0] == '%') {
            n = sh_atoi(argv[i] + 1);
            if (n < 1 || n > njobs) { uprintf("kill: no such job: %s\n", argv[i]); ++i; continue; }
            pid = job_pid[n - 1];
        } else pid = sh_atoi(argv[i]);
        if (ukill(pid, sig) < 0) uprintf("kill: no such task: %s\n", argv[i]);
        ++i;
    }
}

static void sh_waitall() {
    int i, st;
    i = 0;
    while (i < njobs) {
        st = uwait(job_pid[i]);
        uprintf("[%d] %s done (%d)\n", i + 1, job_name[i], st);
        free(job_argv[i]);
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
    // "wait PID" waits for any task, not only this shell's jobs: the
    // root archive unpacks in the background at boot, and vfsload
    // names its task so a script can wait for the files.
    if (!ustrcmp(argv[0], "wait")) {
        if (argc > 1) uprintf("[%s] done (%d)\n", argv[1], uwait(sh_atoi(argv[1])));
        else sh_waitall();
        return 1;
    }
    // `jobs` could show you a background task and nothing could stop
    // it. A job number is the useful handle -- "%1" the way a real
    // shell spells it -- with a raw pid accepted for anything the
    // shell did not start.
    if (!ustrcmp(argv[0], "kill")) { sh_kill(argv, argc); return 1; }
    if (!ustrcmp(argv[0], "help")) {
        uprintf("c4ix-sh: cmd [args] [< in] [> out] [| cmd ...] [&]\n");
        uprintf("builtins: exit jobs wait kill cd pwd rm mv help\n");
        uprintf("  kill [-SIG] %%JOB | PID   default signal 15 (TERM)\n");
        uprintf("  wait [PID]                every job, or task PID\n");
        uprintf("  rm [-r] [-f] PATH...      -r: a directory and all in it\n");
        uprintf("wildcards: * and ? in a word's last part, as in rm /build/bin/*\n");
        return 1;
    }
    if (!ustrcmp(argv[0], "cd")) {
        char *dir;
        dir = (argc > 1) ? argv[1] : "/";
        if (uchdir(dir) < 0) uprintf("cd: no such directory: %s\n", dir);
        return 1;
    }
    // the syscalls Files and the IDE use, for the console too
    if (!ustrcmp(argv[0], "rm")) { sh_rm(argv, argc); return 1; }
    if (!ustrcmp(argv[0], "mv")) {
        if (argc != 3) uprintf("usage: mv PATH NEWNAME (in the same directory)\n");
        else if (urename(argv[1], argv[2]) < 0) uprintf("mv: cannot rename %s\n", argv[1]);
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
    char *cand;
    int i, k, argc, background, hasnext, pid, fd, st, form;
    int prev_read, saved0, saved1;
    int p[2];
    int pids[ARG_MAX];
    int npids;

    background = 0;
    if (nw > 0) {
        if (w[nw - 1] == tok_amp) { background = 1; --nw; }
    }
    if (!nw) return 0;
    glob_used = 0;          // the last line's names are copied where they were kept

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
            else if (argc < ARG_MAX - 1) {
                k = sh_wild(w[i]) ? sh_glob(w[i], argv, argc, ARG_MAX - 1) : 0;
                if (k) argc = argc + k;
                else { argv[argc] = w[i]; ++argc; }
            }
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
        pid = -1;
        form = 0;
        while (form < 4 && pid < 0) {
            cand = sh_candidate(argv[0], form);
            if (cand) pid = spawn(cand, argc, spawned);
            ++form;
        }

        udup2(saved0, STDIN);  uclose(saved0);
        udup2(saved1, STDOUT); uclose(saved1);
        if (prev_read >= 0) uclose(prev_read);
        if (hasnext) { uclose(p[1]); prev_read = p[0]; }
        else prev_read = -1;

        if (pid < 0) { uprintf("sh: cannot run %s\n", argv[0]); free(spawned); return 1; }
        if (background) sh_addjob(pid, spawned);
        else { pids[npids] = pid; ++npids; free(spawned); }
    }

    // Foreground: wait for every stage; the last one gives the status.
    i = 0;
    while (i < npids) { st = uwait(pids[i]); ++i; }
    return st;
}

// With no script the shell reads fd 0, which is a person, so it
// prints a prompt carrying the working directory.
//
// The prompt ends with a NEWLINE rather than sitting on the input
// line. There is no flush in this system -- the only thing that
// reliably pushes output out is the newline itself -- so a trailing
// "$ " would still be sitting in a buffer while the shell waited for
// a key, and the session would look hung. c4sh does the same.
//
// The desktop's Terminal reads the shell through a pipe, where
// every write arrives at once, so it passes -p and the prompt sits on
// the input line: "c4ix:/$ ls".
static void sh_prompt() {
    char cwd[128];
    if (!interactive) return;
    if (ugetcwd(cwd, 128) > 0) uprintf(inline_prompt ? "c4ix:%s$ " : "c4ix:%s$\n", cwd);
    else uprintf(inline_prompt ? "c4ix$ " : "c4ix$\n");
}

int main(int argc, char **argv) {
    char line[LINE_MAX];
    char *words[WORD_MAX];
    int nw, len, i, k, first;

    script_fd = STDIN;
    interactive = 1;
    // -p first, for a prompt on the input line (above); then -e CMD, as
    // often as wanted: lines run first, as if typed at the prompt
    // (init --desktop: "desktop &", then "top")
    first = 1;
    if (first < argc && !ustrcmp(argv[first], "-p")) { inline_prompt = 1; ++first; }
    i = first;
    while (i + 1 < argc && !ustrcmp(argv[i], "-e")) i = i + 2;
    if (i < argc) {
        interactive = 0;
        if ((script_fd = uopen(argv[i], O_RD)) < 0) {
            ufprintf(STDERR, "sh: cannot open %s\n", argv[i]);
            return 1;
        }
    }
    if (interactive)
        uprintf("c4ix-sh -- 'help' for builtins, 'exit' or end-of-file to leave\n");
    i = first;
    while (i + 1 < argc && !ustrcmp(argv[i], "-e") && !sh_exiting) {
        k = 0;
        while (argv[i + 1][k] && k < LINE_MAX - 1) { line[k] = argv[i + 1][k]; ++k; }
        line[k] = 0;
        sh_prompt();
        if (interactive) uprintf("%s\n", line);
        nw = sh_split(line, words, WORD_MAX);
        if (nw) sh_runline(words, nw);
        i = i + 2;
    }

    while (!sh_exiting) {
        sh_prompt();
        if ((len = sh_getline(line, LINE_MAX)) < 0) {
            if (interactive) uprintf("\n");
            break;
        }
        if (!len) continue;
        nw = sh_split(line, words, WORD_MAX);
        if (!nw) continue;
        sh_runline(words, nw);
    }

    if (njobs) sh_waitall();
    if (script_fd != STDIN) uclose(script_fd);
    return sh_status;
}

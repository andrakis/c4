// C4IX userland: top. Takes a snapshot of the machine's accounting each
// second and reports what each task did BETWEEN the snapshots, which is
// the number that actually answers "what is using the machine" -- a
// total since boot mostly tells you which task is oldest.
//
// The shares are cpustat.h's, the same as System Monitor's: each task's
// own cycles, System for the kernel's work on everyone's behalf, and the
// idle task for the rest, naps included -- so a screen adds up to 100.
//
// Runs until Ctrl-C, redrawing in place once a second; `top N` stops
// after N screens. A screen fits the terminal, LINES by COLUMNS from the
// environment or 24 by 80 (C4IX's own terminals are 25 by 80): when the
// tasks do not all fit, the busiest are shown and System counts the
// rest, and lines stop short of the last column, so nothing wraps. It is
// drawn over the last -- home, each line, erase-to-end-of-line (the last
// line's erases the screen's rest) -- and ends at the start of the next
// row, so it never scrolls, and what follows top starts a line of its own.

#include "c4ix_user.h"
#include "cpustat.h"

enum { SAMPLE_MS = 1000, ROWS_MAX = 200, COLS_MAX = 250 };

static struct cpusample samples[2];
static int pct[CPU_MAXT];
static int top_busy[CPU_MAXT];   // thousands of cycles in the interval
static int top_shown[CPU_MAXT];  // whether its row is on the screen

static char *fb;                 // the screen, written at once
static int fb_n, rows, cols;     // its length; the terminal's size
static int ln_count, ln_len, ln_on;  // lines so far; this one's length

static char *top_state(int st) {
    if (st == TS_READY) return "ready";
    if (st == TS_RUNNING) return "run";
    if (st == TS_ZOMBIE) return "zombie";
    if (st == TS_WAITING) return "wait";
    if (st == TS_BLOCKED) return "block";
    if (st == TS_SLEEPING) return "sleep";
    if (st == TS_VFORK) return "vfork";
    return "?";
}

static int atoi_(char *s) {
    int n;
    n = 0;
    while (*s >= '0' && *s <= '9') { n = n * 10 + (*s - '0'); ++s; }
    return n;
}

// NAME's number in the environment (LINES=30), or DEF
static int env_num(char *name, int def) {
    char **e;
    char *s, *p;
    e = uenviron();
    while (e && *e) {
        s = *e; p = name;
        while (*p && *s == *p) { ++s; ++p; }
        if (!*p && *s == '=' && s[1] >= '0' && s[1] <= '9')
            return atoi_(s + 1);
        ++e;
    }
    return def;
}

// ESC [ FIN: H home, K erase the line's rest, J the screen's
static void fb_esc(int fin) {
    fb[fb_n] = 27; fb[fb_n + 1] = '['; fb[fb_n + 2] = fin;
    fb_n = fb_n + 3;
}

// A line, if the screen has room: the last row stays empty, for the
// cursor. Past the bottom, what is written to it is dropped.
static void ln_begin() {
    ln_len = 0;
    ln_on = ln_count < rows - 1;
    if (ln_on) ++ln_count;
}

static void ln_end() {
    if (ln_on) { fb_esc('K'); fb[fb_n] = '\n'; ++fb_n; }
    ln_on = 0;
}

// a character, while the line is short of the last column
static void ln_c(int c) {
    if (ln_on && ln_len < cols - 1) { fb[fb_n] = c; ++fb_n; ++ln_len; }
}

static void ln_s(char *s) { while (*s) { ln_c(*s); ++s; } }

// V's digits into BUF, at least MIN (zeros in front); returns how many
static int digits(int v, int min, char *buf) {
    char tmp[24];
    int n, i;
    n = 0;
    if (v < 0) { buf[0] = '-'; return 1 + digits(-v, min, buf + 1); }
    while (v || n < min || n == 0) { tmp[n] = '0' + v % 10; ++n; v = v / 10; }
    i = 0;
    while (n) { --n; buf[i] = tmp[n]; ++i; }
    buf[i] = 0;
    return i;
}

static void ln_d(int v, int min) { char b[24]; digits(v, min, b); ln_s(b); }

// S left-aligned in WIDTH, or right-aligned and then a space: the
// columns libc4ix's upadstr, upadhdr and upadnum make
static void ln_pad(char *s, int width, int right) {
    int n;
    n = ustrlen(s);
    if (right) while (n < width) { ln_c(' '); ++n; }
    ln_s(s);
    if (right) ln_c(' ');
    else while (n < width) { ln_c(' '); ++n; }
}

static void ln_num(int v, int width) {
    char b[24];
    digits(v, 1, b);
    ln_pad(b, width, 1);
}

// C cycles, scaled as upadcycles2 scales them: 999, 1.000k ... 1.000P
static void ln_cycles(long long c, int width) {
    char b[32];
    char *sfx;
    long long d;
    int n, k, f;
    sfx = "kMGTP";
    d = 1;
    n = -1;
    while (n < 4 && c >= d * 1000) { d = d * 1000; ++n; }
    k = digits((int)(c / d), 1, b);
    if (n >= 0) {
        f = (int)(c / (d / 1000)) - (int)(c / d) * 1000;   // 3 decimals
        b[k] = '.';
        k = k + 1 + digits(f, 3, b + k + 1);
        b[k] = sfx[n]; b[k + 1] = 0;
    }
    ln_pad(b, width, 1);
}

// Which of the N records get a row: all, or the busiest ROOM -- by
// share, then by cycles, which tell apart the many at 0%. They keep
// their order. Returns how many are left out.
static int top_pick(int n, int room) {
    int k, best, got;
    if (room < 0) room = 0;
    k = 0;
    while (k < n) { top_shown[k] = n <= room; ++k; }
    got = 0;
    while (n > room && got < room) {
        best = -1;
        k = 0;
        while (k < n) {
            if (!top_shown[k] && (best < 0 || pct[k] > pct[best] ||
                (pct[k] == pct[best] && top_busy[k] > top_busy[best])))
                best = k;
            ++k;
        }
        top_shown[best] = 1;
        ++got;
    }
    return n > room ? n - room : 0;
}

int main(int argc, char **argv) {
    struct cpusample *a, *b, *s;
    struct cpustat_task *r;
    int iters, it, k, n, sys, usage, ks, idle, smp, more;
    char note[24];

    iters = (argc > 1) ? atoi_(argv[1]) : 0;   // 0: until Ctrl-C
    rows = env_num("LINES", 24);
    cols = env_num("COLUMNS", 80);
    rows = rows < 4 ? 4 : rows > ROWS_MAX ? ROWS_MAX : rows;
    cols = cols < 16 ? 16 : cols > COLS_MAX ? COLS_MAX : cols;
    fb = (char *)ualloc(rows * (cols + 4) + 16);   // lines, erases, home
    if (!fb) { uprintf("top: out of memory\n"); return 1; }

    // a baseline first, or the first screen would be totals since boot
    a = &samples[0];
    b = &samples[1];
    cpu_sample(a);
    it = 0;
    while (!iters || it < iters) {
        umsleep(SAMPLE_MS);      // asleep: top itself costs almost nothing
        cpu_sample(b);
        usage = cpu_shares(a, b, pct, &sys);
        n = b->h.ntasks;

        fb_n = 0;
        ln_count = 0;
        fb_esc('H');
        ln_begin();
        ln_s("top: "); ln_d(n, 1);
        ln_s(" tasks, CPU usage "); ln_d(usage, 1);
        ln_s("% -- Ctrl-C to stop");
        ln_end();
        // several CPUs: each one's load, and which runs each task
        smp = b->h.ncpu > 1;
        if (smp) {
            ln_begin();
            ln_s("CPUs: ");
            k = 0;
            while (k < b->h.ncpu && k < CPUSTAT_CPUS) {
                ln_c(' '); ln_d(k, 1); ln_c(':');
                ln_d(cpu_load(a, b, k), 2); ln_c('%');
                ++k;
            }
            ln_end();
        }
        ln_begin();
        ln_pad("ID", 4, 1); ln_pad("PPID", 5, 1); ln_pad("STATE", 8, 0);
        if (smp) ln_pad("CPU", 4, 1);
        ln_pad("PRIV", 8, 0); ln_pad("CPU%", 7, 1);
        ln_pad("INTERVAL", 11, 1); ln_pad("TOTAL", 11, 1); ln_s(" NAME");
        ln_end();

        // every task's cycles, shown or not: System has the kernel's part
        ks = 0;
        idle = 0;
        k = 0;
        while (k < n) {
            top_busy[k] = cpu_kdelta(a, b, k, 0);
            if (cpu_isidle(b, b->t[k].id)) {
                idle = 1;
                top_busy[k] = top_busy[k] + cpu_kdelta(a, b, k, 1);
            } else ks = ks + cpu_kdelta(a, b, k, 1);
            ++k;
        }
        // the rows left, less System's and the note under it
        more = top_pick(n, rows - 1 - ln_count - 1 - (idle ? 0 : 1));

        k = 0;
        while (k < n) {
            r = &b->t[k];
            if (top_shown[k]) {
                ln_begin();
                ln_num(r->id, 4); ln_num(r->parent, 5);
                ln_pad(top_state(r->state), 8, 0);
                if (smp && r->cpu >= 0) ln_num(r->cpu, 4);
                else if (smp) ln_pad("-", 4, 1);
                ln_pad(r->privs ? "user" : "kernel", 8, 0); ln_num(pct[k], 7);
                ln_cycles((long long)top_busy[k] * 1000, 11);
                if (cpu_isidle(b, r->id)) ln_cycles(r->user + r->kernel, 11);
                else ln_cycles(r->user, 11);
                ln_c(' '); ln_s(r->name);
                ln_end();
            }
            ++k;
        }
        // the kernel's work for all of them, as System Monitor shows it,
        // and under the IDs the tasks there was no room for
        ln_begin();
        note[0] = 0;
        if (more) {
            note[0] = '+';
            ustrcpy(note + 1 + digits(more, 1, note + 1), " more");
        }
        ln_pad(note, 5 + 6 + 8 + 8 + (smp ? 5 : 0), 0);     // up to CPU%
        ln_num(sys, 7);
        ln_cycles((long long)ks * 1000, 11);
        ln_pad("", 12, 0);
        ln_s(" System");
        ln_end();
        if (!idle) {
            ln_begin(); ln_s("(no idle task in the snapshot)"); ln_end();
        }
        fb[fb_n - 2] = 'J';      // the last line's erase: the screen's rest
        write(STDOUT, fb, fb_n);

        // this sample becomes the baseline for the next one
        s = a; a = b; b = s;
        ++it;
    }
    return 0;
}

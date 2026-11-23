// C4IX userland: top. The tasks, what each did in the last interval and
// in all, a screen a second until q or Ctrl-C: Linux's top, as far as
// C4IX has the numbers for it.
//
// The shares are cpustat.h's, as System Monitor's are: each task's own
// cycles; sy, the kernel's work on everyone's behalf; and id, the idle
// tasks' with the naps -- so us, sy and id make 100.
//
// Keys are read as they are typed (ESC _ T r, which the console and the
// desktop's Terminal both take), and they are Linux top's: P, M, T and N
// sort by %CPU, memory, time and ID, R the other way; V shows the tasks
// as a tree; i hides those that did nothing; 1 gives each CPU a line;
// k kills a task, r renices one, d or s sets the delay; Space redraws
// now; the arrows, PgUp, PgDn, Home and End scroll; h lists them; q
// quits. `top N` stops after N screens, and `top -b` writes its screens
// one after another for a file, all its tasks, and reads no keys.
//
// A screen fits LINES by COLUMNS from the environment, or 24 by 80, and
// lines stop short of the last column, so nothing wraps. It is drawn
// over the last -- home, each line, erase-to-end-of-line (the last
// line's erases the screen's rest) -- and ends at the start of the next
// row, so it never scrolls, and what follows top starts a line of its own.

#include "c4ix_user.h"
#include "cpustat.h"

enum { ROWS_MAX = 200, COLS_MAX = 250, KEYBUF = 64, ANSWER = 16 };
enum { SORT_CPU = 0, SORT_MEM = 1, SORT_TIME = 2, SORT_PID = 3 };
enum { K_UP = 256, K_DOWN, K_LEFT, K_RIGHT, K_HOME, K_END, K_PGUP, K_PGDN };
enum { T_SIGINT = 2, T_SIGKILL = 9, T_SIGTERM = 15 };

static struct cpusample samples[2];
static int pct[CPU_MAXT];
static int busy[CPU_MAXT];       // thousands of cycles in the interval
static int nices[CPU_MAXT];      // UNICE_NONE: the kernel's or an idle task
static int list[CPU_MAXT];       // the records, in the order shown
static int depth[CPU_MAXT];      // ... and, as a tree, how deep each is
static int seen[CPU_MAXT];
static int nlist, us, sy, id, usage;

static int rows, cols, batch, delay, sortby, reverse, tree, hideidle, percpu;
static int scroll, help, have;   // have: a screen's numbers to draw again
static int ask, askpid;          // a question on the message line
static char answer[ANSWER];
static char msg[80];

static char *fb;                 // the screen, written at once
static int fb_n;
static int ln_count, ln_len, ln_on;   // lines so far; this one's length

static char kb[KEYBUF];          // keys read, not yet taken
static int kb_n, in_eof;


// /proc/meminfo's "swap N" and "swapfree N" (KB); 0 where there is none
static int swap_kb, swapfree_kb;
static char swap_buf[2048];
static int swap_val(char *p, char *name) {
    int k;
    while (*p) {
        k = 0;
        while (name[k] && p[k] == name[k]) ++k;
        if (!name[k]) return uatoi(p + k);
        while (*p && *p != 10) ++p;
        if (*p) ++p;
    }
    return 0;
}
static void swap_read() {
    int n;
    swap_kb = 0; swapfree_kb = 0;
    if ((n = uslurp("/proc/meminfo", swap_buf, 2047)) <= 0) return;
    swap_buf[n] = 0;
    swap_kb = swap_val(swap_buf, "swap ");
    swapfree_kb = swap_val(swap_buf, "swapfree ");
}

static int atoi_(char *s) {
    int n, neg;
    n = 0; neg = 0;
    if (*s == '-') { neg = 1; ++s; }
    while (*s >= '0' && *s <= '9') { n = n * 10 + (*s - '0'); ++s; }
    return neg ? 0 - n : n;
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

// ---- the screen ----------------------------------------------------------------

// ESC [ and S: a VT100 sequence, which a batch screen does without
static void fb_esc(char *s) {
    if (batch) return;
    fb[fb_n] = 27; fb[fb_n + 1] = '['; fb_n = fb_n + 2;
    while (*s) { fb[fb_n] = *s; ++fb_n; ++s; }
}

// A line, if the screen has room: the last row stays empty, for the
// cursor. Past the bottom, what is written to it is dropped.
static void ln_begin() {
    ln_len = 0;
    ln_on = ln_count < rows - 1;
    if (ln_on) ++ln_count;
}

static void ln_end() {
    if (ln_on) { fb_esc("K"); fb[fb_n] = '\n'; ++fb_n; }
    ln_on = 0;
}

static void ln_esc(char *s) { if (ln_on) fb_esc(s); }

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
    if (v < 0) { buf[0] = '-'; return 1 + digits(0 - v, min, buf + 1); }
    while (v || n < min || n == 0) { tmp[n] = '0' + v % 10; ++n; v = v / 10; }
    i = 0;
    while (n) { --n; buf[i] = tmp[n]; ++i; }
    buf[i] = 0;
    return i;
}

static void ln_d(int v, int min) { char b[24]; digits(v, min, b); ln_s(b); }

// S left-aligned in WIDTH, or right-aligned and then a space
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

// spaces up to column C
static void ln_to(int c) { while (ln_len < c && ln_len < cols - 1) ln_c(' '); }

// C cycles, scaled: 999, 1.000k ... 1.000P
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

// C cycles as Linux's TIME+, minutes:seconds.hundredths, at CPMS cycles
// a millisecond; as cycles if that was never measured
static void ln_time(long long c, int cpms, int width) {
    char b[32];
    long long cs;
    int k;
    if (cpms <= 0) { ln_cycles(c, width); return; }
    cs = c / ((long long)cpms * 10);
    k = digits((int)(cs / 6000), 1, b);
    b[k] = ':';
    k = k + 1 + digits((int)(cs / 100 % 60), 2, b + k + 1);
    b[k] = '.';
    digits((int)(cs % 100), 2, b + k + 1);
    ln_pad(b, width, 1);
}

// ---- keys ----------------------------------------------------------------------

static void key_take(int n) {
    int i;
    i = 0;
    while (i + n < kb_n) { kb[i] = kb[i + n]; ++i; }
    kb_n = kb_n - n;
}

// What has been typed, without waiting: 0 if nothing
static int key_fill() {
    int n;
    if (in_eof || batch || kb_n >= KEYBUF) return 0;
    n = uavail(STDIN);
    if (n < 0) { in_eof = 1; return 0; }
    if (n == 0) return 0;
    // as much as there is room for: the console's uavail says only 1, and
    // an arrow's ESC [ B read a byte at a time would be Esc, [ and B
    n = uread(STDIN, kb + kb_n, KEYBUF - kb_n);
    if (n <= 0) { in_eof = 1; return 0; }
    kb_n = kb_n + n;
    return n;
}

// The next key typed, or -1: a character (Enter as 13), or one of K_*
// for the sequences a terminal sends for the arrows and the rest. An
// ESC on its own is Esc.
static int key_next() {
    int c, i, f, p, k;
    if (!kb_n) return -1;
    c = kb[0] & 255;
    if (c != 27 || kb_n < 2 || (kb[1] != '[' && kb[1] != 'O')) {
        key_take(1);
        return c == 10 ? 13 : c;
    }
    i = 2;
    while (i < kb_n && (kb[i] < 64 || kb[i] > 126)) ++i;
    if (i >= kb_n) { key_take(kb_n); return 27; }      // cut short: Esc
    f = kb[i];
    p = kb[2];
    key_take(i + 1);
    k = 0;
    if (f == 'A') k = K_UP;
    if (f == 'B') k = K_DOWN;
    if (f == 'C') k = K_RIGHT;
    if (f == 'D') k = K_LEFT;
    if (f == 'H') k = K_HOME;
    if (f == 'F') k = K_END;
    if (f == '~' && (p == '1' || p == '7')) k = K_HOME;
    if (f == '~' && (p == '4' || p == '8')) k = K_END;
    if (f == '~' && p == '5') k = K_PGUP;
    if (f == '~' && p == '6') k = K_PGDN;
    return k;
}

// A key, or -1 once the machine's clock reaches UNTIL. A Terminal's keys
// come down a pipe, which uevwait wakes for; the console's are looked at
// every 50 ms.
static int key_wait(int until) {
    int k, left;
    while (1) {
        if ((k = key_next()) >= 0) return k;
        if (key_fill() > 0) continue;
        left = until - __time();
        if (left <= 0) return -1;
        if (batch || in_eof) umsleep(left);
        else uevwait(left < 50 ? left : 50, 0, 1);
    }
    return -1;
}

// ---- the numbers ---------------------------------------------------------------

static int t_state(int st) {
    if (st == TS_READY || st == TS_RUNNING) return 'R';
    if (st == TS_SLEEPING) return 'S';
    if (st == TS_WAITING || st == TS_VFORK) return 'W';
    if (st == TS_BLOCKED) return 'D';
    if (st == TS_ZOMBIE) return 'Z';
    return '?';
}

static long long t_total(struct cpustat_task *r) { return r->user + r->kernel; }

// Does record I go before record J, as the tasks are sorted?
static int t_before(struct cpusample *b, int i, int j) {
    long long x, y;
    int d;
    d = 0;
    if (sortby == SORT_CPU) {
        d = pct[i] - pct[j];
        if (!d) d = busy[i] - busy[j];
    } else if (sortby == SORT_MEM) d = b->t[i].mem - b->t[j].mem;
    else if (sortby == SORT_TIME) {
        x = t_total(&b->t[i]); y = t_total(&b->t[j]);
        d = x > y ? 1 : x < y ? 0 - 1 : 0;
    }
    if (!d || sortby == SORT_PID) d = b->t[j].id - b->t[i].id;   // the lower ID first
    return reverse ? d < 0 : d > 0;
}

// As a tree: record K, then its children, each sorted, under it
static void t_visit(struct cpusample *b, int *order, int n, int k, int d) {
    int i;
    seen[k] = 1;
    list[nlist] = k; depth[nlist] = d; ++nlist;
    i = 0;
    while (i < n) {
        if (!seen[order[i]] && b->t[order[i]].parent == b->t[k].id) t_visit(b, order, n, order[i], d + 1);
        ++i;
    }
}

// The rows in the order shown: sorted, or as a tree; with i, only the
// tasks that ran
static void t_order(struct cpusample *b) {
    int order[CPU_MAXT];
    int n, k, i, j, x;
    n = b->h.ntasks;
    k = 0;
    while (k < n) {
        x = k;
        i = k;
        while (i > 0 && t_before(b, x, order[i - 1])) { order[i] = order[i - 1]; --i; }
        order[i] = x;
        ++k;
    }
    nlist = 0;
    if (tree) {
        k = 0;
        while (k < n) { seen[k] = 0; ++k; }
        // the roots -- a parent not in the snapshot -- then any the
        // kernel and idle tasks, each the other's parent, leave
        k = 0;
        while (k < n) {
            x = order[k];
            if (!seen[x] && (cpu_find(b, b->t[x].parent) < 0 || b->t[x].parent == b->t[x].id)) t_visit(b, order, n, x, 0);
            ++k;
        }
        k = 0;
        while (k < n) { if (!seen[order[k]]) t_visit(b, order, n, order[k], 0); ++k; }
    } else {
        k = 0;
        while (k < n) { list[k] = order[k]; depth[k] = 0; ++k; }
        nlist = n;
    }
    if (hideidle) {
        j = 0;
        k = 0;
        while (k < nlist) {
            x = list[k];
            if (busy[x] || b->t[x].state == TS_RUNNING) { list[j] = x; depth[j] = depth[k]; ++j; }
            ++k;
        }
        nlist = j;
    }
}

// What the screen shows of samples A and B
static void t_compute(struct cpusample *a, struct cpusample *b) {
    int k, n;
    usage = cpu_shares(a, b, pct, &sy);
    n = b->h.ntasks;
    us = 0; id = 0;
    k = 0;
    while (k < n) {
        busy[k] = cpu_kdelta(a, b, k, 0);
        if (cpu_isidle(b, b->t[k].id)) {
            busy[k] = busy[k] + cpu_kdelta(a, b, k, 1);
            id = id + pct[k];
            nices[k] = UNICE_NONE;
        } else {
            us = us + pct[k];
            nices[k] = b->t[k].privs ? unice(b->t[k].id, UNICE_ASK) : UNICE_NONE;
        }
        ++k;
    }
    // each task's share is rounded on its own: 26 at 3.8% would add to 104
    if (us + sy + id > 100) us = 100 - sy - id;
    if (us < 0) us = 0;
    t_order(b);
}

// ---- drawing -------------------------------------------------------------------

static void d_uptime(int ms) {
    int s;
    s = ms / 1000;
    ln_d(s / 3600, 1); ln_c(':'); ln_d(s / 60 % 60, 2); ln_c(':'); ln_d(s % 60, 2);
}

static void d_count(int v, char *what) { ln_c(' '); ln_pad("", v < 10 ? 2 : v < 100 ? 1 : 0, 0); ln_d(v, 1); ln_c(' '); ln_s(what); }

static void d_header(struct cpusample *a, struct cpusample *b) {
    int k, n, run, slp, wt, blk, zom, st, w, f, load;
    n = b->h.ntasks;
    ln_begin();
    ln_s("top - up "); d_uptime(b->h.ms);
    ln_s(", "); ln_d(b->h.ncpu, 1); ln_s(b->h.ncpu > 1 ? " CPUs" : " CPU");
    ln_s(", usage "); ln_d(usage, 1); ln_s("%");
    if (!batch) { ln_to(cols - 1 - 22); ln_s("h for help, q to quit"); }
    ln_end();
    run = 0; slp = 0; wt = 0; blk = 0; zom = 0;
    k = 0;
    while (k < n) {
        st = t_state(b->t[k].state);
        if (st == 'R') ++run;
        if (st == 'S') ++slp;
        if (st == 'W') ++wt;
        if (st == 'D') ++blk;
        if (st == 'Z') ++zom;
        ++k;
    }
    ln_begin();
    ln_s("Tasks:"); d_count(b->h.total > n ? b->h.total : n, "total,");
    d_count(run, "running,"); d_count(slp, "sleeping,"); d_count(wt, "waiting,");
    d_count(blk, "blocked,"); d_count(zom, "zombie");
    ln_end();
    ln_begin();
    ln_s("%Cpu(s):"); d_count(us, "us,"); d_count(sy, "sy,"); d_count(id, "id");
    if (b->h.ncpu > 1 && !percpu) {
        ln_s("   ");
        k = 0;
        while (k < b->h.ncpu && k < CPUSTAT_CPUS) {
            ln_s(" "); ln_d(k, 1); ln_c(':'); ln_d(cpu_load(a, b, k), 2); ln_c('%');
            ++k;
        }
    }
    ln_end();
    // with 1: a line and a bar for each CPU
    k = 0;
    while (percpu && k < b->h.ncpu && k < CPUSTAT_CPUS) {
        load = cpu_load(a, b, k);
        ln_begin();
        ln_s("%Cpu"); ln_d(k, 1); ln_to(7); ln_s(": "); ln_pad("", load < 10 ? 2 : load < 100 ? 1 : 0, 0);
        ln_d(load, 1); ln_s("% [");
        w = cols - 1 - ln_len - 1;
        f = w > 0 ? load * w / 100 : 0;
        while (w > 0) { ln_c(f > 0 ? '|' : ' '); --f; --w; }
        ln_c(']');
        ln_end();
        ++k;
    }
    if (b->h.heap > 0) {
        ln_begin();
        ln_s("KiB Mem:"); ln_c(' '); ln_d(b->h.heap / 1024, 1); ln_s(" total, ");
        ln_d(b->h.heapused / 1024, 1); ln_s(" used, ");
        ln_d((b->h.heap - b->h.heapused) / 1024, 1); ln_s(" free");
        ln_end();
    }
    // swap, from /proc/meminfo (KB), where the MMU has it
    swap_read();
    if (swap_kb > 0) {
        ln_begin();
        ln_s("KiB Swap:"); ln_c(' '); ln_d(swap_kb, 1); ln_s(" total, ");
        ln_d(swap_kb - swapfree_kb, 1); ln_s(" used, ");
        ln_d(swapfree_kb, 1); ln_s(" free");
        ln_end();
    }
}

// The message line: a question being answered, what the last command
// did, or which of the rows are on the screen
static void d_message(int room) {
    int last;
    ln_begin();
    if (ask) {
        if (ask == 'k') { ln_s("PID to kill [default pid = "); ln_d(askpid, 1); ln_s("]: "); }
        if (ask == 'K') { ln_s("Kill PID "); ln_d(askpid, 1); ln_s(" with signal [15]: "); }
        if (ask == 'r') { ln_s("PID to renice [default pid = "); ln_d(askpid, 1); ln_s("]: "); }
        if (ask == 'R') { ln_s("Renice PID "); ln_d(askpid, 1); ln_s(" to value (-10 to 10): "); }
        if (ask == 'd') { ln_s("Change delay from "); ln_d(delay / 1000, 1); ln_c('.'); ln_d(delay / 100 % 10, 1); ln_s(" to: "); }
        ln_s(answer);
    } else if (msg[0]) ln_s(msg);
    else if (nlist > room && room > 0) {
        last = scroll + room < nlist ? scroll + room : nlist;
        ln_s("tasks "); ln_d(scroll + 1, 1); ln_c('-'); ln_d(last, 1); ln_s(" of "); ln_d(nlist, 1);
        ln_s(", scroll with Up, Down, PgUp and PgDn");
    }
    ln_end();
}

static void d_help() {
    ln_begin(); ln_s("Keys, as in Linux's top:"); ln_end();
    ln_begin(); ln_end();
    ln_begin(); ln_s("  P M T N  sort by %CPU, memory, time or ID      R  the other way"); ln_end();
    ln_begin(); ln_s("  V        the tasks as a tree                    i  only those that ran"); ln_end();
    ln_begin(); ln_s("  1        a line for each CPU                    Space  redraw now"); ln_end();
    ln_begin(); ln_s("  k        kill a task (signal 2, 9 or 15)        r  renice one, -10 to 10"); ln_end();
    ln_begin(); ln_s("  d or s   the delay, in seconds                  q  quit (or Ctrl-C)"); ln_end();
    ln_begin(); ln_s("  Up, Down, PgUp, PgDn, Home, End  scroll"); ln_end();
    ln_begin(); ln_end();
    ln_begin(); ln_s("S, the state: R running or ready, S sleeping, W waiting for a child,"); ln_end();
    ln_begin(); ln_s("D blocked, Z a zombie. USER is the task's privilege, kernel or user;"); ln_end();
    ln_begin(); ln_s("RES the KiB it holds; sy in %Cpu(s) is the kernel's work for them all."); ln_end();
    ln_begin(); ln_end();
    ln_begin(); ln_s("Any key goes back."); ln_end();
}

static void d_columns(int smp) {
    ln_begin();
    ln_esc("7m");
    ln_pad("PID", 5, 1); ln_pad("PPID", 5, 1); ln_pad("USER", 7, 0);
    ln_pad("NI", 3, 1); ln_pad("S", 2, 0);
    if (smp) ln_pad("P", 2, 1);
    ln_pad("RES", 6, 1); ln_pad("%CPU", 4, 1); ln_pad("%MEM", 4, 1);
    ln_pad("TIME+", 9, 1); ln_s("COMMAND");
    if (!batch) ln_to(cols - 1);
    ln_esc("m");
    ln_end();
}

static void d_row(struct cpusample *b, int k, int d, int smp) {
    struct cpustat_task *r;
    int i;
    r = &b->t[k];
    ln_begin();
    if (r->state == TS_RUNNING) ln_esc("1m");
    ln_num(r->id, 5); ln_num(r->parent, 5);
    ln_pad(r->privs ? "user" : "kernel", 7, 0);
    if (nices[k] == UNICE_NONE) ln_pad("-", 3, 1); else ln_num(nices[k], 3);
    ln_c(t_state(r->state)); ln_c(' ');
    if (smp && r->cpu >= 0) ln_num(r->cpu, 2);
    else if (smp) ln_pad("-", 2, 1);
    ln_num(r->mem / 1024, 6); ln_num(pct[k], 4);
    if (b->h.heap > 0) ln_num((int)((long long)r->mem * 100 / b->h.heap), 4);
    else ln_pad("-", 4, 1);
    ln_time(t_total(r), b->h.cpms, 9);
    if (d > 0) {
        i = 1;
        while (i < d) { ln_s("  "); ++i; }
        ln_s("`- ");
    }
    ln_s(r->name);
    if (r->state == TS_RUNNING) ln_esc("m");
    ln_end();
}

// The screen, for samples A and B (t_compute's numbers)
static void draw(struct cpusample *a, struct cpusample *b) {
    int k, room, smp;
    fb_n = 0;
    ln_count = 0;
    fb_esc("H");
    d_header(a, b);
    smp = b->h.ncpu > 1;
    // the rows left under the message line and the columns' names
    room = rows - 1 - ln_count - 2;
    if (batch) room = nlist;
    if (scroll > nlist - room) scroll = nlist - room;
    if (scroll < 0) scroll = 0;
    d_message(room);
    if (help) d_help();
    else {
        d_columns(smp);
        k = scroll;
        while (k < nlist && k < scroll + room) { d_row(b, list[k], depth[k], smp); ++k; }
    }
    if (batch) { fb[fb_n] = '\n'; ++fb_n; }
    else fb[fb_n - 2] = 'J';      // the last line's erase: the screen's rest
    write(STDOUT, fb, fb_n);
}

// ---- commands ------------------------------------------------------------------

// raw keys on (1) or lines again (0): console.c and the Terminal's term.c
static void tty_raw(int on) {
    char b[6];
    if (batch) return;
    b[0] = 27; b[1] = '_'; b[2] = 'T'; b[3] = on ? 'r' : 'c'; b[4] = 27; b[5] = '\\';
    write(STDOUT, b, 6);
}

// A question's answer: Enter takes it (empty, the default), Esc drops it
static void answered(struct cpusample *b) {
    int v, n, s, ok;
    char *p;
    v = atoi_(answer);
    n = ustrlen(answer);
    msg[0] = 0;
    if (ask == 'k' || ask == 'r') {
        if (n) askpid = v;
        if (cpu_find(b, askpid) < 0) { ustrcpy(msg, "no such task"); ask = 0; return; }
        ask = ask == 'k' ? 'K' : 'R';
        answer[0] = 0;
        return;
    }
    if (ask == 'K') {
        s = n ? v : T_SIGTERM;
        if (s != T_SIGINT && s != T_SIGKILL && s != T_SIGTERM) ustrcpy(msg, "signals are 2, 9 and 15");
        else {
            ok = ukill(askpid, s) >= 0;
            p = ustrcpy(msg, ok ? "sent signal " : "could not send signal ") - 1;
            p = p + digits(s, 1, p);
            p = ustrcpy(p, " to task ") - 1;
            digits(askpid, 1, p);
        }
    }
    if (ask == 'R' && n) {
        if (v < 0 - 10 || v > 10) ustrcpy(msg, "nice is -10 to 10");
        else if (unice(askpid, v) == UNICE_NONE) ustrcpy(msg, "that task cannot be reniced");
    }
    if (ask == 'd' && n) {
        // seconds, to a tenth
        v = 0;
        p = answer;
        while (*p >= '0' && *p <= '9') { v = v * 10 + (*p - '0'); ++p; }
        v = v * 1000;
        if (*p == '.' && p[1] >= '0' && p[1] <= '9') v = v + (p[1] - '0') * 100;
        delay = v < 100 ? 100 : v;
    }
    ask = 0;
}

// One key. 1 if it wants a new sample now, 2 to quit; the screen is
// drawn again either way.
static int command(struct cpusample *b, int c) {
    int n, room;
    if (ask) {
        n = ustrlen(answer);
        if (c == 13) answered(b);
        else if (c == 27) { ask = 0; msg[0] = 0; }
        else if ((c == 127 || c == 8) && n) answer[n - 1] = 0;
        else if (((c >= '0' && c <= '9') || c == '-' || c == '.') && n < ANSWER - 1) { answer[n] = c; answer[n + 1] = 0; }
        return 0;
    }
    if (help) { help = 0; return 0; }
    msg[0] = 0;
    room = rows - 8;
    if (room < 1) room = 1;
    if (c == 'q') return 2;
    if (c == ' ' || c == 13) return 1;
    if (c == 'h' || c == '?') help = 1;
    if (c == 'P') sortby = SORT_CPU;
    if (c == 'M') sortby = SORT_MEM;
    if (c == 'T') sortby = SORT_TIME;
    if (c == 'N') sortby = SORT_PID;
    if (c == 'R') reverse = !reverse;
    if (c == 'V') tree = !tree;
    if (c == 'i') hideidle = !hideidle;
    if (c == '1') percpu = !percpu;
    if (c == K_UP) scroll = scroll - 1;
    if (c == K_DOWN) scroll = scroll + 1;
    if (c == K_PGUP) scroll = scroll - room;
    if (c == K_PGDN) scroll = scroll + room;
    if (c == K_HOME) scroll = 0;
    if (c == K_END) scroll = CPU_MAXT;
    if (c == 'k' || c == 'r' || c == 'd' || c == 's') {
        ask = c == 's' ? 'd' : c;
        answer[0] = 0;
        askpid = nlist > scroll ? b->t[list[scroll]].id : 0;
    }
    if (c == 'P' || c == 'M' || c == 'T' || c == 'N' || c == 'R' || c == 'V' || c == 'i') t_order(b);
    return 0;
}

int main(int argc, char **argv) {
    struct cpusample *a, *b, *s;
    int iters, it, k, c, until, got;

    iters = 0;   // 0: until q or Ctrl-C
    delay = 1000;
    k = 1;
    while (k < argc) {
        if (!ustrcmp(argv[k], "-b")) batch = 1;
        else if (!ustrcmp(argv[k], "-n") && k + 1 < argc) { ++k; iters = atoi_(argv[k]); }
        else if (!ustrcmp(argv[k], "-d") && k + 1 < argc) { ++k; delay = atoi_(argv[k]) * 1000; }
        else if (argv[k][0] >= '0' && argv[k][0] <= '9') iters = atoi_(argv[k]);
        else { uprintf("usage: top [-b] [-d seconds] [-n screens] [screens]\n"); return 1; }
        ++k;
    }
    if (delay < 100) delay = 100;
    rows = env_num("LINES", 24);
    cols = env_num("COLUMNS", 80);
    rows = rows < 8 ? 8 : rows > ROWS_MAX ? ROWS_MAX : rows;
    cols = cols < 40 ? 40 : cols > COLS_MAX ? COLS_MAX : cols;
    if (batch) rows = ROWS_MAX;
    fb = (char *)ualloc(rows * (cols + 16) + 64);   // lines, erases, colours
    if (!fb) { uprintf("top: out of memory\n"); return 1; }
    tty_raw(1);

    // a baseline first, or the first screen would be totals since boot
    a = &samples[0];
    b = &samples[1];
    cpu_sample(a);
    it = 0;
    c = 0;
    while (c != 2 && (!iters || it < iters)) {
        // until the next sample: keys, each drawn at once
        until = __time() + delay;
        got = 0;
        while (!got && (k = key_wait(until)) >= 0) {
            c = command(a, k);   // a: the newest sample, the one drawn
            if (c == 2) break;
            if (c == 1) got = 1;
            else if (have) draw(b, a);
        }
        if (c == 2) break;
        cpu_sample(b);
        t_compute(a, b);
        if (!ask) msg[0] = 0;
        draw(a, b);
        have = 1;
        // this sample becomes the baseline for the next one; the pair
        // just drawn is (b, a) until the next sample
        s = a; a = b; b = s;
        ++it;
    }
    tty_raw(0);
    return 0;
}

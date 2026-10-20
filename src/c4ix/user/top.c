// C4IX userland: top. Samples the task table twice and reports what
// each task did BETWEEN the samples, which is the number that
// actually answers "what is using the machine" -- a total since boot
// mostly tells you which task is oldest.
//
// Runs until Ctrl-C, redrawing in place once a second, as top does;
// `top N` stops after N screens. (It used to stop after three: it was
// written before the console could be interrupted.)

#include "c4ix_user.h"

enum { MAX_TASKS = 32, SAMPLE_MS = 1000 };

static int prev_id[MAX_TASKS];
static int prev_hi[MAX_TASKS], prev_lo[MAX_TASKS];
static int nprev;

static char *top_state(int st) {
    if (st == TS_READY) return "ready";
    if (st == TS_RUNNING) return "run";
    if (st == TS_ZOMBIE) return "zombie";
    if (st == TS_WAITING) return "wait";
    if (st == TS_BLOCKED) return "block";
    if (st == TS_SLEEPING) return "sleep";
    return "?";
}

// The thousands of cycles a task ran since the previous sample (all of
// them, if it is new). Cycles come as two words, hi billions and lo
// (sched.c); a delta in cycles overflows 32 bits at 2.1G, and times 100
// for a percentage at 21M -- a fifth of a second -- so the arithmetic is
// in kilocycles, which a 32-bit word holds for 24 days at 100 MHz.
static int top_kdelta(int id, int hi, int lo) {
    int i;
    i = 0;
    while (i < nprev && prev_id[i] != id) ++i;
    if (i == nprev) return hi * 1000000 + lo / 1000;
    return (hi - prev_hi[i]) * 1000000 + lo / 1000 - prev_lo[i] / 1000;
}

// one pass over the task table into snap; the number of tasks
static int top_sample(int *snap) {
    int info[TASKINFO_WORDS];
    int i, j, n, base;
    n = 0;
    i = 0;
    while (utaskinfo(i, info)) {
        if (n < MAX_TASKS) {
            base = n * TASKINFO_WORDS;
            j = 0;
            while (j < TASKINFO_WORDS) { snap[base + j] = info[j]; ++j; }
            ++n;
        }
        ++i;
    }
    return n;
}

// this sample becomes the baseline for the next one
static void top_keep(int *snap, int n) {
    int i, base;
    nprev = n;
    i = 0;
    while (i < n) {
        base = i * TASKINFO_WORDS;
        prev_id[i] = snap[base];
        prev_lo[i] = snap[base + 6];
        prev_hi[i] = snap[base + 7];
        ++i;
    }
}

// Let the machine run for a second between samples, asleep rather than
// spinning, so top itself costs almost nothing.
static void top_settle() {
    umsleep(SAMPLE_MS);
}

// Clear the screen and home the cursor (ESC [ 2 J, ESC [ H), so each
// sample replaces the last instead of scrolling.
static void top_home() {
    uputchar(27); uputchar('['); uputchar('2'); uputchar('J');
    uputchar(27); uputchar('['); uputchar('H');
}

static int atoi_(char *s) {
    int n;
    n = 0;
    while (*s >= '0' && *s <= '9') { n = n * 10 + (*s - '0'); ++s; }
    return n;
}

int main(int argc, char **argv) {
    // One flat snapshot: sampling the table twice would let the
    // tasks run in between, and the percentages would not add up.
    int snap[MAX_TASKS * TASKINFO_WORDS];
    int iters, it, i, n, delta, busy, pct, base;

    iters = (argc > 1) ? atoi_(argv[1]) : 0;   // 0: until Ctrl-C

    // a baseline first, or the first screen is totals since boot
    top_keep(snap, top_sample(snap));
    it = 0;
    while (!iters || it < iters) {
        top_settle();

        // one pass: copy the whole table, then work only from the
        // copy, so every number printed belongs to the same instant
        n = top_sample(snap);
        busy = 0;
        i = 0;
        while (i < n) {
            base = i * TASKINFO_WORDS;
            delta = top_kdelta(snap[base], snap[base + 7], snap[base + 6]);
            if (delta > 0) busy = busy + delta;
            ++i;
        }
        if (!busy) busy = 1;

        top_home();
        uprintf("top: %d tasks, ", n);
        upadcycles2(busy / 1000000, busy % 1000000 * 1000, 0);
        uprintf("cycles in the last second -- Ctrl-C to stop\n");
        upadhdr("ID", 4);
        upadhdr("PPID", 5);
        upadstr("STATE", 8);
        upadstr("PRIV", 8);
        upadhdr("CPU%", 7);
        upadhdr("INTERVAL", 11);
        upadhdr("TOTAL", 11);
        uprintf(" NAME\n");

        i = 0;
        while (i < n) {
            base = i * TASKINFO_WORDS;
            delta = top_kdelta(snap[base], snap[base + 7], snap[base + 6]);
            if (delta < 0) delta = 0;
            pct = (delta * 100 + busy / 2) / busy;
            if (pct > 100) pct = 100;
            upadnum(snap[base], 4);
            upadnum(snap[base + 1], 5);
            upadstr(top_state(snap[base + 2]), 8);
            upadstr(snap[base + 3] ? "user" : "kernel", 8);
            upadnum(pct, 7);
            upadcycles2(delta / 1000000, delta % 1000000 * 1000, 11);
            upadcycles2(snap[base + 7], snap[base + 6], 11);
            uprintf(" %s\n", (char *)(snap + base + 8));
            ++i;
        }

        top_keep(snap, n);
        ++it;
    }
    return 0;
}

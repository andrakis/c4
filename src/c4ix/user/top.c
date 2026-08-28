// C4IX userland: top. Samples the task table twice and reports what
// each task did BETWEEN the samples, which is the number that
// actually answers "what is using the machine" -- a total since boot
// mostly tells you which task is oldest.
//
// Takes an iteration count (default 3) rather than running forever:
// there is no non-blocking console read yet, so nothing could type
// 'q' to stop it.

#include "c4ix_user.h"

enum { MAX_TASKS = 32, SAMPLE_CYCLES = 200000 };

static int prev_id[MAX_TASKS];
static int prev_cyc[MAX_TASKS];
static int nprev;

static char *top_state(int st) {
    if (st == TS_READY) return "ready";
    if (st == TS_RUNNING) return "run";
    if (st == TS_ZOMBIE) return "zombie";
    if (st == TS_WAITING) return "wait";
    if (st == TS_BLOCKED) return "block";
    return "?";
}

// cycles this task had at the previous sample, or -1 if it is new
static int top_prev(int id) {
    int i;
    i = 0;
    while (i < nprev) {
        if (prev_id[i] == id) return prev_cyc[i];
        ++i;
    }
    return -1;
}

// Let the machine run for a while without spinning on it: yield
// repeatedly until enough cycles have passed for the sample to mean
// something.
static void top_settle() {
    int t0;
    t0 = ucycles();
    while (ucycles() - t0 < SAMPLE_CYCLES) uyield();
}

static int atoi_(char *s) {
    int n;
    n = 0;
    while (*s >= '0' && *s <= '9') { n = n * 10 + (*s - '0'); ++s; }
    return n;
}

int main(int argc, char **argv) {
    int info[TASKINFO_WORDS];
    // One flat snapshot: sampling the table twice would let the
    // tasks run in between, and the percentages would not add up.
    int snap[MAX_TASKS * TASKINFO_WORDS];
    int iters, it, i, j, n, was, delta, busy, pct, base;

    iters = (argc > 1) ? atoi_(argv[1]) : 3;
    if (iters < 1) iters = 1;

    it = 0;
    while (it < iters) {
        top_settle();

        // one pass: copy the whole table, then work only from the
        // copy, so every number printed belongs to the same instant
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
        busy = 0;
        i = 0;
        while (i < n) {
            base = i * TASKINFO_WORDS;
            was = top_prev(snap[base]);
            // The low word carries at a billion (sched.c), so a delta
            // across a carry looks negative. It is one billion more.
            delta = (was < 0) ? snap[base + 6] : snap[base + 6] - was;
            if (delta < 0) delta = delta + 1000000000;
            if (delta > 0) busy = busy + delta;
            ++i;
        }
        if (!busy) busy = 1;

        uprintf("\ntop: %d tasks, ", n);
        upadcycles(busy, 0);
        uprintf("cycles this interval\n");
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
            was = top_prev(snap[base]);
            // The low word carries at a billion (sched.c), so a delta
            // across a carry looks negative. It is one billion more.
            delta = (was < 0) ? snap[base + 6] : snap[base + 6] - was;
            if (delta < 0) delta = delta + 1000000000;
            if (delta < 0) delta = 0;
            pct = delta * 100 / busy;
            upadnum(snap[base], 4);
            upadnum(snap[base + 1], 5);
            upadstr(top_state(snap[base + 2]), 8);
            upadstr(snap[base + 3] ? "user" : "kernel", 8);
            upadnum(pct, 7);
            upadcycles(delta, 11);
            upadcycles2(snap[base + 7], snap[base + 6], 11);
            uprintf(" %s\n", (char *)(snap + base + 8));
            ++i;
        }

        // this sample becomes the baseline for the next one
        nprev = n;
        i = 0;
        while (i < n) {
            base = i * TASKINFO_WORDS;
            prev_id[i] = snap[base];
            prev_cyc[i] = snap[base + 6];
            ++i;
        }
        ++it;
    }
    return 0;
}

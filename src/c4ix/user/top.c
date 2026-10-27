// C4IX userland: top. Takes a snapshot of the machine's accounting each
// second and reports what each task did BETWEEN the snapshots, which is
// the number that actually answers "what is using the machine" -- a
// total since boot mostly tells you which task is oldest.
//
// The shares are cpustat.h's, the same as System Monitor's: each task's
// own cycles, System for the kernel's work on everyone's behalf, and the
// idle task for the rest, naps included -- so a screen adds up to 100.
//
// Runs until Ctrl-C, redrawing in place once a second, as top does;
// `top N` stops after N screens. (It used to stop after three: it was
// written before the console could be interrupted.)

#include "c4ix_user.h"
#include "cpustat.h"

enum { SAMPLE_MS = 1000 };

static struct cpusample samples[2];
static int pct[CPU_MAXT];

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

// thousands of cycles, as upadcycles2's billions and the rest
static void top_kcycles(int k, int width) {
    upadcycles2(k / 1000000, k % 1000000 * 1000, width);
}

// a long long count of cycles, the same way
static void top_cycles(long long c, int width) {
    int hi;
    hi = (int)(c / 1000000000);
    upadcycles2(hi, (int)(c - (long long)hi * 1000000000), width);
}

int main(int argc, char **argv) {
    struct cpusample *a, *b, *s;
    struct cpustat_task *r;
    int iters, it, k, sys, usage, ks, idle, smp;

    iters = (argc > 1) ? atoi_(argv[1]) : 0;   // 0: until Ctrl-C

    // a baseline first, or the first screen would be totals since boot
    a = &samples[0];
    b = &samples[1];
    cpu_sample(a);
    it = 0;
    while (!iters || it < iters) {
        top_settle();
        cpu_sample(b);
        usage = cpu_shares(a, b, pct, &sys);

        top_home();
        uprintf("top: %d tasks, CPU usage %d%% -- Ctrl-C to stop\n", b->h.ntasks, usage);
        // several CPUs: each one's load, and which runs each task
        smp = b->h.ncpu > 1;
        if (smp) {
            uprintf("CPUs: ", b->h.ncpu);
            k = 0;
            while (k < b->h.ncpu && k < CPUSTAT_CPUS) {
                uprintf(" %d:%02d%%", k, cpu_load(a, b, k));
                ++k;
            }
            uprintf("\n");
        }
        upadhdr("ID", 4);
        upadhdr("PPID", 5);
        upadstr("STATE", 8);
        if (smp) upadhdr("CPU", 4);
        upadstr("PRIV", 8);
        upadhdr("CPU%", 7);
        upadhdr("INTERVAL", 11);
        upadhdr("TOTAL", 11);
        uprintf(" NAME\n");

        ks = 0;
        idle = 0;
        k = 0;
        while (k < b->h.ntasks) {
            r = &b->t[k];
            upadnum(r->id, 4);
            upadnum(r->parent, 5);
            upadstr(top_state(r->state), 8);
            if (smp) { if (r->cpu >= 0) upadnum(r->cpu, 4); else upadhdr("-", 4); }
            upadstr(r->privs ? "user" : "kernel", 8);
            upadnum(pct[k], 7);
            if (cpu_isidle(b, r->id)) {
                idle = 1;
                top_kcycles(cpu_kdelta(a, b, k, 0) + cpu_kdelta(a, b, k, 1), 11);
                top_cycles(r->user + r->kernel, 11);
            } else {
                ks = ks + cpu_kdelta(a, b, k, 1);
                top_kcycles(cpu_kdelta(a, b, k, 0), 11);
                top_cycles(r->user, 11);
            }
            uprintf(" %s\n", r->name);
            ++k;
        }
        // the kernel's work for all of them, as System Monitor shows it
        upadstr("", 5 + 6 + 8 + 8 + (smp ? 5 : 0));     // under CPU%
        upadnum(sys, 7);
        top_kcycles(ks, 11);
        upadstr("", 12);
        uprintf(" System\n");
        if (!idle) uprintf("(no idle task in the snapshot)\n");

        // this sample becomes the baseline for the next one
        s = a; a = b; b = s;
        ++it;
    }
    return 0;
}

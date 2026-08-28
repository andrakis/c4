// bb_pit.c - c4bb programmable interrupt timer test.
//
// The cycle interrupt fires every N INSTRUCTIONS, so a kernel using it
// has to guess how many instructions a second is on whatever host it
// woke up on -- and gets it wrong, because that number is different on
// every machine and changes with the weather. The PIT fires every N
// REAL MILLISECONDS instead, raising the same TRAP_HARD_IRQ, so a
// kernel that already has a handler needs no new one.
//
// It is a device register (0x1a0), not an opcode: writing it is an
// ordinary store, so even a base-c4 program can arm it. Reading the
// real clock (0x19c) is likewise a load. What this test needs above
// EXIT is only what any trap handler needs -- ITH, and TLEV to return.
//
// The report is structure, not timings: ticks delivered, and that they
// arrived spread out in real time rather than all at once. Exact
// spacing depends on how fast the host is and is not a property of the
// machine.
enum { CONF_CYCLE_INTERRUPT_INTERVAL, CONF_CYCLE_INTERRUPT_HANDLER };
enum { TRAP_HARD_IRQ = 1 };
enum { DEV_RTC = 0x19c, DEV_PIT = 0x1a0 };
enum { PERIOD = 100, WANT = 4 };

int ticks, first_ms, last_ms;
int spins;      // where the unmasked busywork below goes, so it stays

int rtc () { return *(int *)DEV_RTC; }

void tick (int trap, int ins, int mode, int a, int *bp, int *sp, int *returnpc) {
    if (trap != TRAP_HARD_IRQ) { printf("pit: unexpected trap %d\n", trap); return; }
    ++ticks;
    if (ticks == 1) first_ms = rtc();
    last_ms = rtc();
    if (ticks >= WANT) *(int *)DEV_PIT = 0;      // disarm from inside
}

// Wait for WANT ticks. `mask` makes the wait loop hide from the timer
// and unhide again on every pass, the way a busy kernel does.
// `guard` bounds the wait so a machine that delivers no ticks fails
// promptly rather than hanging. It is passed in because the two passes
// do very different amounts of work per turn: the masked one carries
// two device writes and a short inner loop, so the same count would
// take it a hundred times longer to give up.
int collect (int mask, int guard) {
    int j;

    ticks = 0;
    *(int *)DEV_PIT = PERIOD;
    while (ticks < WANT && --guard) {
        if (mask) {
            *(int *)DEV_PIT = 0;         // into a critical path
            *(int *)DEV_PIT = PERIOD;    // and straight back out of it
            // Then ordinary work, unmasked -- the proportion matters.
            // The machine only looks at the host clock every 4096
            // cycles, so a loop that were masked for most of its length
            // could miss ticks for that reason instead of the one under
            // test, and a kernel does not spend most of its life inside
            // a critical path either.
            j = 0;
            while (++j < 64) spins = spins + j;
        }
    }
    *(int *)DEV_PIT = 0;
    return ticks;
}

int report (char *what, int got) {
    int span;

    if (got < WANT) {
        printf("pit: FAILED, %s got only %d of %d ticks\n", what, got, WANT);
        return 1;
    }
    span = last_ms - first_ms;
    // Three intervals between four ticks; allow a wide band, because
    // this is measuring the host and not the machine.
    if (span < PERIOD || span > PERIOD * 12) {
        printf("pit: FAILED, %s spread %d ticks over %dms\n", what, got, span);
        return 1;
    }
    printf("pit: %s -- %d ticks over %dms, real time\n", what, got, span);
    return 0;
}

int main () {
    int bad;

    if (rtc() < 0) { printf("pit: no real-time clock\n"); return 1; }

    __c4_configure(CONF_CYCLE_INTERRUPT_HANDLER, (int)&tick);
    printf("pit: %dms apart requested\n", PERIOD);

    bad = report("free running", collect(0, 200000000));

    // Masking must not restart the countdown.
    //
    // A kernel hides from its own timer on the way into every critical
    // path and comes back out on the way from it, which on a busy
    // kernel happens many times per tick. If writing the interval
    // always started a new countdown, such a kernel would never be
    // interrupted again -- it would mask itself to death, and the
    // symptom would read as "the scheduler stopped", not as a timer
    // bug. So mask and unmask far faster than the period, and require
    // the same ticks in the same real time anyway.
    bad = bad + report("masked every pass", collect(1, 500000));

    return bad;
}

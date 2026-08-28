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

int rtc () { return *(int *)DEV_RTC; }

void tick (int trap, int ins, int mode, int a, int *bp, int *sp, int *returnpc) {
    if (trap != TRAP_HARD_IRQ) { printf("pit: unexpected trap %d\n", trap); return; }
    ++ticks;
    if (ticks == 1) first_ms = rtc();
    last_ms = rtc();
    if (ticks >= WANT) *(int *)DEV_PIT = 0;      // disarm from inside
}

int main () {
    int spins, guard, span;

    if (rtc() < 0) { printf("pit: no real-time clock\n"); return 1; }

    ticks = 0;
    __c4_configure(CONF_CYCLE_INTERRUPT_HANDLER, (int)&tick);
    *(int *)DEV_PIT = PERIOD;

    // Spin until the ticks arrive, with a bound so a machine that has
    // no PIT fails the test rather than hanging in it.
    spins = 0;
    guard = 200000000;
    while (ticks < WANT && --guard) ++spins;

    *(int *)DEV_PIT = 0;
    if (ticks < WANT) { printf("pit: only %d of %d ticks\n", ticks, WANT); return 1; }

    span = last_ms - first_ms;
    printf("pit: %d ticks, %dms apart requested\n", ticks, PERIOD);
    // Three intervals between four ticks; allow a wide band, because
    // this is measuring the host and not the machine.
    if (span >= PERIOD && span <= PERIOD * 12)
        printf("pit: spread over %dms, real time\n", span);
    else
        printf("pit: FAILED, %d ticks spread over %dms\n", ticks, span);
    return 0;
}

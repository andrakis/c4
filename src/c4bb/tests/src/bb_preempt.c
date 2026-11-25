// bb_preempt.c - c4bb cycle-interrupt (preemption) parity test.
//
// Phase 1 (C4KE style): the handler re-arms the interval itself via
// __c4_configure, since firing zeroes it (the only interrupt mask).
//
// Phase 2 (C4IX style): CONF_TRAP_RESTORES_INTERVAL makes the saved
// interval part of the trap context; the handler declares the extra
// leading `interval` parameter (the bp+9 slot) and assigns it, and
// TLEV re-arms on return - no explicit reconfigure.
//
// The output reports structure only (ticks delivered, that the
// mainline progressed between them), not spin counts: PRTF is one
// host call under native c4m but hundreds of firmware instructions
// under c4bb, so absolute cycle positions - and thus exact spin
// counts - legitimately differ. Cycle-exactness of the plain
// instruction stream is covered by cycles.c4r.

enum { CONF_CYCLE_INTERRUPT_INTERVAL, CONF_CYCLE_INTERRUPT_HANDLER,
       CONF_PRIVS, CONF_TRAP_RESTORES_INTERVAL };
enum { TRAP_ILLOP, TRAP_HARD_IRQ };
enum { INTERVAL = 5000 };

int ticks, want;

void tick_rearm (int trap, int ins, int mode, int a, int *bp, int *sp, int *returnpc) {
    if (trap != TRAP_HARD_IRQ) { printf("phase1: unexpected trap %d\n", trap); return; }
    ++ticks;
    if (ticks < want)
        __c4_configure(CONF_CYCLE_INTERRUPT_INTERVAL, INTERVAL);
}

void tick_restore (int interval, int trap, int ins, int mode, int a, int *bp, int *sp, int *returnpc) {
    if (trap != TRAP_HARD_IRQ) { printf("phase2: unexpected trap %d\n", trap); return; }
    ++ticks;
    // the interval parameter is the saved bp+9 slot; TLEV restores it
    interval = (ticks < want) ? INTERVAL : 0;
}

int spin_until (int target) {
    int i;
    i = 0;
    while (ticks < target) ++i;
    return i;
}

int main (int argc, char **argv) {
    int i;

    // phase 1: re-arm from inside the handler
    ticks = 0; want = 3;
    __c4_configure(CONF_CYCLE_INTERRUPT_HANDLER, (int)&tick_rearm);
    __c4_configure(CONF_CYCLE_INTERRUPT_INTERVAL, INTERVAL);
    i = spin_until(3);
    printf("phase1: ticks=%d progressed=%d\n", ticks, i > 0);

    // phase 2: interval restored from the trap frame
    ticks = 0; want = 2;
    __c4_configure(CONF_TRAP_RESTORES_INTERVAL, 1);
    __c4_configure(CONF_CYCLE_INTERRUPT_HANDLER, (int)&tick_restore);
    __c4_configure(CONF_CYCLE_INTERRUPT_INTERVAL, INTERVAL);
    i = spin_until(2);
    printf("phase2: ticks=%d progressed=%d\n", ticks, i > 0);

    // mask off, restore defaults
    __c4_configure(CONF_CYCLE_INTERRUPT_INTERVAL, 0);
    __c4_configure(CONF_TRAP_RESTORES_INTERVAL, 0);
    printf("bb_preempt done\n");
    return 0;
}

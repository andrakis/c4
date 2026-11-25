// bb_mbox.c - the mailbox probe: parity when absent, echo when fitted.
//
// Without the region (native c4m, or c4bb booted without one) the only
// output is "not fitted" -- the same bytes on both hosts, which is what
// test-c4bb's `check` pins. With it, every inbox frame comes back with
// its type | 256 and each payload word + 1; a type-0 frame ends the run;
// the host's interrupt is counted through a cycle handler, exactly the
// way a kernel would take it. Built with include/c4bb_mbox.h passed as
// a source (build-images.sh).

enum { TRAP_HARD_IRQ = 1, HIRQ_MBOX = 2, CONF_CYCLE_INTERRUPT_HANDLER = 1 };

int irqs;

void on_irq (int trap, int ins, int mode, int a, int *bp, int *sp, int *returnpc) {
    if (trap == TRAP_HARD_IRQ && ins == HIRQ_MBOX) ++irqs;
}

int main (int argc, char **argv) {
    int *f, n, i, echoed, full, payload[64];
    if (!mb_init()) { printf("mbox: not fitted\n"); return 0; }
    printf("mbox: fitted, in cap %d, out cap %d\n", mb_in[MB_CAP], mb_out[MB_CAP]);
    __c4_configure(CONF_CYCLE_INTERRUPT_HANDLER, (int)&on_irq);
    echoed = 0; full = 0;
    while (1) {
        f = mb_poll();
        if (!f) { mb_bell(MB_BELL_IDLE); continue; }
        if (f[1] == 0) { mb_done(f); break; }         // QUIT
        n = f[0] - MB_HDR;
        if (n > 64) n = 64;
        i = 0;
        while (i < n) { payload[i] = f[MB_HDR + i] + 1; i = i + 1; }
        if (!mb_send(f[1] | 256, payload, n)) ++full;
        mb_done(f);
        mb_bell(MB_BELL_REPLY);
        ++echoed;
    }
    printf("mbox: echoed %d frames, outbox full %d times, irqs %d\n", echoed, full, irqs);
    return 0;
}

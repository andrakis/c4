// bb_pm.c - protected-mode parity test.
//
// Entering protected mode is itself done with the trap machinery: a
// handler assigns 1 to its `mode` parameter and TLEV resumes the
// interrupted code protected. A gated opcode (PRTF here) then raises
// TRAP_PM_VIOLATION; the handler runs unprotected (the call site
// drops the mode bit before entry), reports, sets the frame's mode
// slot back to 0 so the resume is unprotected, and plants a fake
// return value in the `a` slot - exactly how C4IX's sys_pmviolation
// emulates c4m opcodes for unmodified binaries.

enum { TRAP_ILLOP, TRAP_HARD_IRQ, TRAP_SOFT_IRQ, TRAP_SIGNAL,
       TRAP_SEGV, TRAP_OPV, TRAP_PM_VIOLATION };
enum { OP_ENTER_PM = 128 };

void handler (int trap, int ins, int mode, int a, int *bp, int *sp, int *returnpc) {
    if (trap == TRAP_ILLOP && ins == OP_ENTER_PM) {
        mode = 1;                 // resume protected
        return;
    }
    if (trap == TRAP_PM_VIOLATION) {
        printf("pm violation: opcode %d (mode was %d)\n", ins, mode);
        mode = 0;                 // resume unprotected
        a = 42;                   // planted "return value"
        return;
    }
    printf("unexpected trap %d ins %d\n", trap, ins);
}

int main (int argc, char **argv) {
    int r;
    install_trap_handler((int *)&handler);

    __c4_opcode(OP_ENTER_PM);
    r = printf("this line must never appear\n");
    printf("after violation: r=%d\n", r);

    install_trap_handler((int *)0);
    printf("bb_pm done\n");
    return 0;
}

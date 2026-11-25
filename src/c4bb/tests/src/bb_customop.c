// bb_customop.c - c4bb trap-machinery parity test.
//
// Like src/tests/test_customop.c, but with custom opcodes in the
// modern range (128+, above INS_SIZE) - the original test predates
// TLEV/DBG and picked 64+, which now collides with real opcodes.
//
// Exercises: ITH install/replace, TRAP_ILLOP delivery for unknown
// opcodes, argument access through the saved-sp frame slot, returning
// values by assigning the handler's `a` parameter (restored by TLEV),
// re-entrant traps (the handler itself invokes a custom opcode), and
// __c4_trap soft interrupts.
//
// Output is deterministic; run under native c4m and c4bb and diff.

enum { TRAP_ILLOP, TRAP_HARD_IRQ, TRAP_SOFT_IRQ };
enum { OP_HELLO = 128, OP_ADD5, OP_SUM2, OP_NESTED, OP_INNER };

int depth;

void trap_handler (int trap, int ins, int mode, int a, int *bp, int *sp, int *returnpc) {
    if (trap == TRAP_ILLOP) {
        if (ins == OP_HELLO) {
            printf("custom: hello (a was %d, depth %d)\n", a, depth);
            return;
        }
        if (ins == OP_ADD5) {
            // sp[0] is the opcode number, sp[1] the first argument
            a = sp[1] + 5;       // assigning a parameter = setting the
            return;              // resumed accumulator (TLEV restores it)
        }
        if (ins == OP_SUM2) {
            a = sp[1] + sp[2];   // args pushed left to right: sp[2] first
            return;
        }
        if (ins == OP_NESTED) {
            ++depth;
            a = __c4_opcode(21, OP_ADD5);   // re-entrant trap
            a = a * 2;
            --depth;
            return;
        }
        printf("unknown custom opcode %d\n", ins);
        return;
    }
    if (trap == TRAP_SOFT_IRQ) {
        printf("soft irq: type slot %d param %d\n", trap, ins);
        return;
    }
    printf("unexpected trap %d ins %d\n", trap, ins);
}

int main (int argc, char **argv) {
    int r, prev;

    depth = 0;
    prev = (int)install_trap_handler((int *)&trap_handler);
    printf("installed, prev=%d\n", prev);

    __c4_opcode(OP_HELLO);
    r = __c4_opcode(10, OP_ADD5);
    printf("add5(10) = %d\n", r);
    r = __c4_opcode(30, 12, OP_SUM2);
    printf("sum2(30,12) = %d\n", r);
    r = __c4_opcode(100, OP_NESTED);
    printf("nested(100) = %d\n", r);

    // soft interrupt through the same handler
    __c4_trap(TRAP_SOFT_IRQ, 77);

    // replacing the handler returns the old one
    prev = (int)install_trap_handler((int *)&trap_handler);
    r = (prev == (int)&trap_handler);
    printf("reinstall returned self: %d\n", r);

    printf("bb_customop done\n");
    return 0;
}

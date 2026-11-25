// C4IX userland: report the VM cycle counter as the first thing a
// user program does. The counterpart of src/tests/cycles.c, which
// does the same under C4KE -- same VM, same counter, same instant,
// so the two numbers are directly comparable.
//
// __c4_cycles() is a c4lc builtin and c4m does not guard it, so this
// reads the counter directly rather than paying for a syscall.

#include "c4ix_user.h"

int main(int argc, char **argv) {
    uprintf("cycles: userland reached at %d cycles\n", __c4_cycles());
    return 0;
}

// badop.c - raise an opcode nobody installed, on purpose.
//
// This exists because of what USED to happen next. C4KE's trap handler
// looked its custom-opcode handler up as
//
//     handler = (int *)(*(custom_opcodes + (ins - CO_BASE)));
//
// with no check that `ins` was inside the table, and then jumped to
// whatever word came back. The bounds check had been written and
// commented out. So an out-of-range opcode read a word from arbitrary
// heap and, whenever that word was non-zero, the kernel adjusted the
// stack by `handler[-1]` and jumped -- into data.
//
// The out-of-range opcodes are not hypothetical: they are POINTERS.
// __c4_opcode leaves its last-evaluated argument in the accumulator
// when a request is never serviced, __u0_ops_init stores whatever
// comes back, and the task then spends the rest of its life raising a
// string address as an opcode -- which is the whole of
// "c4ke: Custom opcode not found: 108138136".
//
// The point of this program is therefore NOT its own death, which is
// expected and correct. It is that the SHELL THAT STARTED IT is still
// there afterwards, and that the trace names a real function.
//
// HONESTY ABOUT WHAT THIS DOES NOT DO: it is not a regression test for
// F17, and was checked against a kernel with the bounds check taken
// back out to be sure. On a quiet boot the word the out-of-range index
// lands on happens to be zero, so the unfixed kernel takes the same
// path and prints the same thing. The read is out of bounds either
// way; whether it hurts depends on what the heap holds, which is why
// the damage only ever showed up under a machine full of live tasks.
// The differential that DOES show it is innerbench under memory
// pressure -- docs/dos-rung-fixes.md F17.
#include "u0.h"

enum {
	// Above CO_BASE + CO_MAX (128 + 128), so no handler can exist for
	// it however many extensions have registered.
	OP_NOBODY_INSTALLED = 9001
};

int main () {
	printf("badop: raising opcode %d, which nothing implements\n", OP_NOBODY_INSTALLED);
	__c4_opcode(OP_NOBODY_INSTALLED);
	// Not reached: C4KE kills a task that raises an opcode it cannot
	// service. If this ever prints, the kernel let it through.
	printf("badop: STILL RUNNING after an unserviced opcode\n");
	return 0;
}

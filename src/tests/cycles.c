// Report the VM cycle counter as the very first thing a user task
// does. Run under any kernel on c4m, this measures the same thing:
// how many cycles the machine spent getting from power-on to running
// userland code.
//
//   ./c4m load-c4r.c -- c4ke.c4r cycles     (C4KE)
//   c4ix prints its own equivalent at handoff to init
//
// The counter is the VM's own, so the number is deterministic and
// comparable across kernels in a way wall-clock milliseconds are not.

#include "c4.h"
#include "c4m.h"

int main (int argc, char **argv) {
	printf("cycles: userland reached at %ld cycles\n", __c4_cycles());
	return 0;
}

// badmem.c -- write one word past a buffer, as a C4KE task.
//
// The companion to src/tests/mpg/mpg_overrun.c, which does the same
// thing as a bare program. This one exists to show the DIVISION between
// the two halves of the report, which is the whole design:
//
//   c4mpg says WHAT was touched -- the nearest region, its permissions,
//   how far past its end the access ran. Only the guard holds a region
//   table.
//
//   C4KE says WHO -- the task id, the name it was started with, its
//   registers and a stack trace through its own symbols -- and then
//   kills that ONE task and carries on. Only the kernel knows a task
//   exists.
//
// Under plain c4m nothing happens at all and this exits 0, which is
// what makes it safe to leave on the disk.
#include "u0.h"

int main () {
	int *p, i;

	if (!(p = malloc(sizeof(int) * 4))) { printf("badmem: no memory\n"); return 1; }
	i = 0;
	while (i < 4) { p[i] = i; ++i; }
	printf("badmem: four words written, all legal so far\n");
	p[4] = 99;                      // <-- one past the end
	printf("badmem: still running -- no guard here\n");
	return 0;
}

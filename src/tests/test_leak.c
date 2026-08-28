// test_leak.c -- does the kernel give a task's memory back?
//
// A program is not obliged to free what it allocated. This one refuses
// to, on purpose, and is then run over and over. Without the kernel
// tracking a task's allocations (docs/task-memory.md) the heap only ever
// shrinks and the run dies partway through; with it, round twelve
// allocates exactly as much as round one.
//
// Run it as the init task: it starts itself as a PRIV_USER child, which
// is the privilege that makes MALC and FREE reach the kernel at all.
#include <u0.h>

enum {
	LEAK_BLOCK  = 262144,   // 256 KB a time
	LEAK_BLOCKS = 24,       // 6 MB per round if it all lands
	LEAK_ROUNDS = 12
};

// The child: allocate until it stops working, free nothing, exit.
static int leak_child () {
	int i, n;
	int *p;

	n = 0;
	i = 0;
	while (i < LEAK_BLOCKS) {
		if (!(p = malloc(LEAK_BLOCK))) break;
		*p = i;                       // touch it, so it is really ours
		n = n + LEAK_BLOCK;
		++i;
	}
	printf("leak: round allocated %d of %d blocks\n", i, LEAK_BLOCKS);
	return i;
}

int main (int argc, char **argv) {
	char *cargv[2];
	int   round, task;

	if (argc > 1) return leak_child();

	cargv[0] = argv[0];
	cargv[1] = "child";
	round = 0;
	while (round < LEAK_ROUNDS) {
		task = kern_user_start_c4r(2, cargv, argv[0], PRIV_USER);
		if (task <= 0) {
			printf("test_leak: could not start round %d\n", round + 1);
			return 1;
		}
		// The child's exit code is how many blocks it managed. A round
		// that got fewer than all of them is the failure this test is
		// looking for: the previous rounds' memory never came back.
		if (await_pid(task) != LEAK_BLOCKS) {
			printf("test_leak: round %d only got some of its memory\n", round + 1);
			return 1;
		}
		++round;
	}
	printf("test_leak: %d rounds done\n", LEAK_ROUNDS);
	return 0;
}

// test_pmputs.c -- does a protected task's puts() reach the screen?
//
// In protected mode PUTS traps to the kernel like PUTC and PRTF do, and
// C4KE's dispatcher (extensions/c4ke_pm.c) has to run it on the task's
// behalf. Until 2026-11-27 it had no PUTS case, so the trap fell through
// to "unhandled" and the task was killed on its first puts().
//
// Run it as the init task: it starts itself as a PRIV_USER child, which
// is the privilege that makes PUTS reach the kernel at all.
#include <u0.h>

enum { CHILD_EXIT = 7 };

static int child () {
	puts("test_pmputs: the child's puts");
	puts("");                          // an empty line is a line too
	puts("test_pmputs: and another");
	return CHILD_EXIT;
}

int main (int argc, char **argv) {
	char *cargv[2];
	int   task, rc;

	if (argc > 1) return child();

	cargv[0] = argv[0];
	cargv[1] = "child";
	task = kern_user_start_c4r(2, cargv, argv[0], PRIV_USER);
	if (task <= 0) {
		printf("test_pmputs: could not start the child\n");
		return 1;
	}
	rc = await_pid(task);
	if (rc != CHILD_EXIT) {
		printf("test_pmputs: the child ended with %d, not %d\n", rc, CHILD_EXIT);
		return 1;
	}
	printf("test_pmputs: ok\n");
	return 0;
}

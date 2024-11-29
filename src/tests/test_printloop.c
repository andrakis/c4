// C4KE test: a printloop
//
// This program will loop a few times, calling schedule.
//
// Usage:
//   ./c4r u0.c test_printloop.c && mv a.c4r test_printloop.c4r

#include "u0.c"

void on_exit_fn () {
	printf("this little function ran after printloop finished\n");
}

int main (int argc, char **argv) {
	int id, count, i;

	id = kern_task_current_id();
	count = 5 * argc;
	printf("printloop.%d starting, count = %d\n", id, count);

	atexit((int *)&on_exit_fn);

	i = 0;
	while(i++ <= count) {
		printf("printloop.%d is up to %d\n", id, i - 1);
		schedule();
	}

	schedule();
	printf("printloop.%d exiting successfully\n", id);
	return 0;
}

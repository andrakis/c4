// C4KE test: infinite loop
// Simply runs in a loop doing nothing.
// Allows for testing process control and interruption.

#include "u0.c"

int run;

static void sighandler_term (int sig) {
	run = 0;
}

int main (int argc, char **argv) {
	int id;
    int altmode, cycles;
	int __some, __vars, __tofix, __c4, __printf;

	run = 1;
	cycles = 0;
	id = pid();
	altmode = (id % 2) == 0;

    // setup SIGTERM handler
    signal(SIGTERM, (int *)&sighandler_term);

	printf("test_infiniteloop.%d: starting infinite loop", id);
	if (altmode) printf(" with wait");
	printf("...\n");

	while(run) {
		id = id;
		if (altmode) {
			if (++cycles % 1000 == 0) sleep(1000);
		}
	}

	return 0;
}

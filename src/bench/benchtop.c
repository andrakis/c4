//
// Very simple test program to launch "bench" and "top" at the same time.
// Can be used as an init process:
//   ./c4m load-c4r.c -- c4ke benchtop
//

#include "u0.c"

int main (int argc, char **argv) {
	int p, top, result;
    int erference; // how many bogus processes to start

    // TODO: allocate argc and argv. For the moment, just reuse our arguments.
	argc = 2;

    // Start 'top'
	argv[0] = "top";
	argv[1] = "-b"; // don't grab foreground
	top = kern_user_start_c4r(argc, argv, "top", PRIV_USER);

	// Start infinite loops
	argc = 1;
    argv[0] = "test_infiniteloop";
    erference = 0;//24;
	if (erference)
		while(erference--)
			kern_user_start_c4r(argc, argv, argv[0], PRIV_USER);

    // Start 'bench' and record it's pid. await on it and return its exit value.
	argc = 2;
	argv[0] = "bench";
	argv[1] = "-C"; // continuous mode
	p = kern_user_start_c4r(argc, argv, "bench", PRIV_USER);
	if (p) {
		// set focus to bench
		c4ke_set_focus(p);
		if ((result = await_pid(p))) {
			printf("benchtop: bench exit with status %d\n", result);
		}
	} else {
		printf("benchtop: failed to launch bench\n");
	}

	if (top)
		kill(top, SIGTERM);
}

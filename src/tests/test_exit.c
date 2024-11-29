// C4KE test: exit
//
// Tests whether a user call to exit quits the entire kernel.

#include "u0.c"

int main (int argc, char **argv) {
	printf("test_exit: calling exit now...\n");
	exit(2);
	return 1;
}

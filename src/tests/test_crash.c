#include <stdio.h>

#include "u0.h"

int do_crash_for_real () {
	__c4_trap(TRAP_SEGV, 0);
	printf("crash didn't happen!\n");
	return 1;
}

int do_crash () {
	return do_crash_for_real();
}

int main (int argc, char **argv) {
	// return 0; // Enable if using destructor crash method
	return do_crash();
}

// Enable to test stack traces on constructor
// static int __attribute__((constructor)) __crash_on_constructor (int *c4r) { do_crash(); }
// Enable to test stack traces on destructor
// static int __attribute__((destructor)) __crash_on_destructor (int *c4r) { do_crash(); }


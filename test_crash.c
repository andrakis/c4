#include <stdio.h>

#include "u0.c"

int do_crash_for_real () {
	__c4_trap(TRAP_SEGV, 0);
	printf("crash didn't happen!\n");
	return 1;
}

int do_crash () {
	return do_crash_for_real();
}

int main (int argc, char **argv) {
	return do_crash();
}

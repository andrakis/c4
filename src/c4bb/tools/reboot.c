// reboot -- ask the breadboard to start again.
//
// Writes c4bb's reset register (include/c4bb.h). The host zeroes the
// arena, places the firmware again, and the BIOS looks at the drives as
// if the power had just come on -- the media survive, which is the
// whole difference between this and switching off.
//
// It exists because of what the climb looks like from the player's
// side: C4DOS builds C4KE and writes it to the disk in drive 1, and
// the next thing that should happen is that C4KE boots. Without this
// the answer is "stop the machine and start it again", which on a
// homebrew computer is a different and much bigger thing.
//
// Board only, and deliberately system-agnostic: it uses no C4DOS API,
// no u0, and no kernel service, so the same image runs as a C4DOS
// transient, a C4KE task and a C4IX program.
#include "c4bb.h"

int main (int argc, char **argv) {
	printf("rebooting...\n");
	bb_reboot();
	// Only reached if the machine has no reset register, which means a
	// c4bb older than docs/c4bb-storage.md M10.
	printf("reboot: this machine has no reset\n");
	return 1;
}

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

// "0" or "1:" -- the same spelling bbsave and install take.
int drivearg (char *s) {
	int c;
	if (!s[0]) return 0 - 1;
	if (s[1] && (s[1] != ':' || s[2])) return 0 - 1;
	c = s[0];
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'A' && c <= 'Z') return c - 'A';
	if (c >= 'a' && c <= 'z') return c - 'a';
	return 0 - 1;
}

int main (int argc, char **argv) {
	int d;

	// `reboot 0` takes the disk out first, which is the whole gesture
	// the climb is built on: install a boot medium in drive 1, take the
	// one you booted from out, and start again. Without the eject the
	// BIOS finds drive 0 still bootable and you come up in exactly the
	// system you were trying to leave.
	if (argc > 1) {
		if ((d = drivearg(argv[1])) < 0) {
			printf("usage: reboot [drive]   -- eject that drive, then restart\n");
			return 1;
		}
		if (d >= bb_drives()) {
			printf("reboot: this machine has %d drive(s), no %d\n", bb_drives(), d);
			return 1;
		}
		printf("ejecting drive %d...\n", d);
		bb_eject(d);
	}
	printf("rebooting...\n");
	bb_reboot();
	// Only reached if the machine has no reset register, which means a
	// c4bb older than docs/c4bb-storage.md M10.
	printf("reboot: this machine has no reset\n");
	return 1;
}

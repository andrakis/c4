// c4bb.h -- the breadboard's drives, from inside the machine.
//
// c4bb's devices are memory-mapped and the CPU reaches them with
// ordinary loads and stores; src/c4bb/fw/fw.c already drives the
// interval and heap registers exactly this way. So a program can write
// to a disk without the VM growing a `write` opcode, which is just as
// well, because C4 has never had one.
//
// The read side (open/read/close) is the microcode's business and is
// reached through the ordinary syscalls. Everything here is the parts
// the microcode does not know about: which drive, how many, whether it
// can be written to, and the write head itself.
//
// BOARD ONLY. These addresses are devices on c4bb and ordinary memory
// anywhere else, so a program that includes this is a program for the
// breadboard -- the same rule dostar and dosload follow. Nothing that
// has to run under native c4m may include it.
//
// See docs/c4bb-storage.md.

enum {
	BB_DRIVE  = 0x13c,   // r/w: selected drive, 0-based
	BB_FD     = 0x124,   // w:   the fd a write applies to (shared with read)
	BB_WNAME  = 0x178,   // w:   create/truncate -> fd, or -1
	BB_WADDR  = 0x17c,   // w:   buffer address
	BB_WLEN   = 0x180,   // w:   write N bytes -> N written
	BB_WCLOSE = 0x184,   // w:   close and flush -> 0, or -1
	BB_COUNT  = 0x188,   // r:   how many drives are attached
	BB_RO     = 0x18c,   // r:   1 if the selected drive is read-only
	BB_EJECT  = 0x190,   // w:   empty the drive whose number is written
	BB_RESCAN = 0x194,   // w:   re-read the drive whose number is written
	BB_RESET  = 0x198,   // w:   soft reset -- back to the BIOS
	BB_RTC    = 0x19c,   // r:   host milliseconds since power-on
	BB_PIT    = 0x1a0    // r/w: tick every N real ms (0 = off)
};

// How many drives this machine has, and which one is selected.
int bb_drives ()          { return *(int *)BB_COUNT; }
int bb_drive ()           { return *(int *)BB_DRIVE; }
void bb_select (int n)    { *(int *)BB_DRIVE = n; }
int bb_readonly ()        { return *(int *)BB_RO; }
void bb_eject (int n)     { *(int *)BB_EJECT = n; }

// A name may carry its own drive -- "1:c4ke.c4r" -- in which case the
// selected drive does not matter. That is C4DOS's own habit and it is
// cheaper than a register.
int bb_create (char *name) {
	*(int *)BB_WNAME = (int)name;
	return *(int *)BB_WNAME;
}

int bb_write (int fd, char *buf, int len) {
	*(int *)BB_FD    = fd;
	*(int *)BB_WADDR = (int)buf;
	*(int *)BB_WLEN  = len;
	return *(int *)BB_WLEN;
}

// Nothing is on the medium until this returns: a written file lands in
// one piece or not at all.
int bb_close (int fd) {
	*(int *)BB_WCLOSE = fd;
	return *(int *)BB_WCLOSE;
}

// The whole of it, which is what every caller actually wants.
int bb_put (char *name, char *buf, int len) {
	int fd;
	if ((fd = bb_create(name)) < 0) return 0 - 1;
	if (len > 0) {
		if (bb_write(fd, buf, len) != len) { bb_close(fd); return 0 - 1; }
	}
	return bb_close(fd);
}

// Ask the machine to start again. Does not return: the host zeroes the
// arena, places the firmware, and the BIOS looks at the drives as if
// the power had just come on -- so an operating system that has just
// written a boot disk can say "now boot it" without anyone touching
// the machine. The media survive; that is the difference between this
// and switching off.
void bb_reboot () {
	*(int *)BB_RESET = 0;
}

// Take the medium out of a drive. The BIOS's wait loop notices.
void bb_rescan (int n) { *(int *)BB_RESCAN = n; }

// The world's clock, not the machine's.
//
// The TIME opcode reports SIMULATED milliseconds, derived from the
// cycle counter: repeatable, which is what tests want, and a fiction,
// because the simulator runs at whatever speed the host manages. This
// is the real one, and it is a register rather than an opcode so that a
// program at the base-c4 rung can read it with an ordinary load.
int bb_rtc () { return *(int *)BB_RTC; }

// Ask for a tick every N real milliseconds, raising the same trap the
// cycle interrupt does -- so a kernel that already has a handler needs
// no new one, and stops having to guess how many cycles a second is on
// this host.
//
// 0 MASKS the timer rather than forgetting it: the deadline stays where
// it was, and re-arming the same interval resumes toward it. That is
// what makes bb_pit(0) safe to use the way a kernel uses it, which is
// on the way into every critical path -- a mask that restarted the
// countdown would let a kernel that masks often enough never tick at
// all. Writing a DIFFERENT interval does start a new countdown.
void bb_pit (int ms) { *(int *)BB_PIT = ms; }
int  bb_pit_get ()   { return *(int *)BB_PIT; }

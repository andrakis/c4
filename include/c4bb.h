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
	BB_EJECT  = 0x190    // w:   empty the drive whose number is written
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

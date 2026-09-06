// install -- write a bootable medium from a manifest.
//
//   RUN install.c4r B:              read install.lst, write drive 1
//   RUN install.c4r B: c4ix.lst     a different manifest
//
// The step the climb was missing. C4DOS builds a kernel into its RAM
// disk (LADDER.BAT), and `bbsave` can put the RAM disk on a medium --
// but a RAM disk is not a boot medium. Half of what the next system
// needs was never in it: the shell, the userland, the archives holding
// the sources for the two rungs above. Those are still on the C4DOS
// floppy, and the floppy is the thing about to be ejected.
//
// So install reads a LIST of what belongs on the medium and fetches
// each name from wherever it actually is. That is one call, because
// C4DOS's own open() checks the RAM disk before the disk -- so a file
// this session compiled shadows the shipped one of the same name,
// which is exactly the precedence a build wants.
//
// The manifest, and it is deliberately almost no grammar at all:
//
//     # a comment
//     boot c4ke.c4r      the image the BIOS is to load, copied like
//                        any other and named in boot.cfg
//     c4sh.c4r           required: absent means the install fails
//     ?c4ke0.c4r         optional: copied if it is there
//
// Board only: it drives c4bb's disk registers (include/c4bb.h), the
// same rung dostar, dosload and bbsave sit on. Strict c4 -- no opcode
// above EXIT, because this runs before the player has extended the
// machine. docs/c4bb-storage.md M11.
#include "c4bb.h"
#include "c4dos.h"

enum {
	CHUNK    = 65536,     // streamed, so no file's size has to be known
	LINE_MAX = 256,
	MAN_MAX  = 65536,     // a manifest larger than this is a mistake
	NAME_MAX = 128,
	DIR_MAX  = 16384      // the c4dos.dir we leave behind
};

char *man;                // the manifest text
int   man_len;
char *buf;                // the streaming buffer
char *dst;                // "N:name", rebuilt per file
char *bootname;           // what boot.cfg will say
char *dirbuf;             // names written, for c4dos.dir
int   dir_len;
int   dest;               // destination drive

int xlen (char *s) { int n; n = 0; while (s[n]) ++n; return n; }

void xcopy (char *d, char *s) { while (*s) { *d = *s; ++d; ++s; } *d = 0; }

// "1:" or "B:" -> the drive number, or -1. bbsave's, verbatim: two
// tools that disagree about what a drive is called would be worse than
// the duplication.
int drivenum (char *s) {
	int c;
	if (!s[0] || s[1] != ':' || s[2]) return 0 - 1;
	c = s[0];
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'A' && c <= 'Z') return c - 'A';
	if (c >= 'a' && c <= 'z') return c - 'a';
	return 0 - 1;
}

// Name a file on the destination drive. Prefixing rather than
// bb_select()ing, because the source drive has to stay selected: DOS
// reads through the board's open(), which resolves against whichever
// drive is current, and the read and the write are interleaved.
char *at_dest (char *name) {
	dst[0] = 'A' + dest;   // one spelling, everywhere the player can see
	dst[1] = ':';
	xcopy(dst + 2, name);
	return dst;
}

// Copy one file, streamed. Returns bytes, or -1.
//
// Streamed rather than slurped so that no file's size has to be known
// in advance and no buffer has to be as large as the largest thing on
// the disk -- c4ix-src.tar is a quarter of a megabyte, and this runs on
// a machine that may have sixteen of them in total.
int copy_one (char *name) {
	int h, fd, n, total;

	if ((h = dos_fopen(name)) < 0) return 0 - 1;
	if ((fd = bb_create(at_dest(name))) < 0) { dos_fclose(h); return 0 - 1; }
	total = 0;
	n = 1;
	while (n > 0) {
		if ((n = dos_fread(h, buf, CHUNK)) > 0) {
			if (bb_write(fd, buf, n) != n) {
				dos_fclose(h);
				bb_close(fd);
				return 0 - 1;
			}
			total = total + n;
		}
	}
	dos_fclose(h);
	if (bb_close(fd) < 0) return 0 - 1;
	return total;
}

// Is it there at all? The first pass asks this of everything required
// before the second pass writes anything.
int present (char *name) {
	int h;
	if ((h = dos_fopen(name)) < 0) return 0;
	dos_fclose(h);
	return 1;
}

void remember (char *name) {
	int n;
	n = xlen(name);
	if (dir_len + n + 1 >= DIR_MAX) return;
	xcopy(dirbuf + dir_len, name);
	dir_len = dir_len + n;
	dirbuf[dir_len] = 10;
	++dir_len;
}

// One line of the manifest into `out`, and the flags with it.
// Returns the offset of the next line, or -1 at the end.
// `kind`: 0 nothing here, 1 required, 2 optional, 3 the boot image.
int next_entry (int at, char *out, int *kind) {
	int n;

	*kind = 0;
	if (at >= man_len) return 0 - 1;
	// Take the line.
	n = 0;
	while (at < man_len && man[at] != 10) {
		if (n < LINE_MAX - 1) { out[n] = man[at]; ++n; }
		++at;
	}
	if (at < man_len) ++at;              // step over the newline
	out[n] = 0;
	// Trim leading and trailing blanks.
	n = 0;
	while (out[n] == ' ' || out[n] == 9) ++n;
	if (n) xcopy(out, out + n);
	n = xlen(out);
	while (n && (out[n - 1] == ' ' || out[n - 1] == 9 || out[n - 1] == 13)) {
		--n;
		out[n] = 0;
	}
	if (!out[0] || out[0] == '#') return at;

	// "boot NAME": the image the BIOS is to load.
	if (out[0] == 'b' && out[1] == 'o' && out[2] == 'o' && out[3] == 't'
	    && (out[4] == ' ' || out[4] == 9)) {
		n = 5;
		while (out[n] == ' ' || out[n] == 9) ++n;
		xcopy(out, out + n);
		*kind = 3;
		return at;
	}
	if (out[0] == '?') { xcopy(out, out + 1); *kind = 2; return at; }
	*kind = 1;
	return at;
}

int main (int argc, char **argv) {
	char *lst, *line;
	int at, kind, keep, missing, n, files, bytes;

	if (argc < 2) {
		printf("usage: install <drive>: [manifest]\n");
		printf("  writes a bootable medium; default manifest is install.lst\n");
		return 1;
	}
	lst = "install.lst";
	if (argc > 2) lst = argv[2];

	if ((dest = drivenum(argv[1])) < 0) {
		printf("install: '%s' is not a drive -- try B:\n", argv[1]);
		return 1;
	}
	// Named with a single digit, so anything past 9 has no spelling
	// here. No machine in this game has ten drives.
	if (dest > 9) {
		printf("install: drive %d is past the last one this can name\n", dest);
		return 1;
	}
	if (dest >= bb_drives()) {
		printf("install: this machine has %d drive(s), no %d\n", bb_drives(), dest);
		return 1;
	}
	if (!dos_readable()) {
		printf("install: this C4DOS cannot be asked for files (needs the read API)\n");
		return 1;
	}
	keep = bb_drive();
	if (dest == keep) {
		printf("install: drive %d is the one we are reading from\n", dest);
		return 1;
	}
	bb_select(dest);
	n = bb_readonly();
	bb_select(keep);
	if (n) {
		printf("install: drive %d is read-only -- nothing to write to\n", dest);
		return 1;
	}

	if (!(man = malloc(MAN_MAX)) || !(buf = malloc(CHUNK))
	    || !(dst = malloc(NAME_MAX + 4)) || !(line = malloc(LINE_MAX))
	    || !(dirbuf = malloc(DIR_MAX))) {
		printf("install: out of memory\n");
		return 1;
	}
	if ((man_len = dos_slurp(lst, man, MAN_MAX)) < 0) {
		printf("install: cannot read the manifest '%s'\n", lst);
		return 1;
	}

	// Pass one: is everything required actually here?
	//
	// Separate from the copying on purpose. A manifest with a typo in
	// it, or a run started before LADDER built the kernel, would
	// otherwise leave a medium that is half an operating system --
	// which boots, and then fails somewhere further in, where the
	// reason is much harder to see.
	printf("install: reading %s\n", lst);
	bootname = 0;
	missing = 0;
	at = 0;
	while ((at = next_entry(at, line, &kind)) >= 0) {
		if (kind == 3) {
			if (!(bootname = malloc(NAME_MAX))) {
				printf("install: out of memory\n");
				return 1;
			}
			xcopy(bootname, line);
		}
		if (kind == 1 || kind == 3) {
			if (!present(line)) {
				printf("  MISSING  %s\n", line);
				++missing;
			}
		}
	}
	if (missing) {
		printf("install: %d file(s) not found -- nothing written\n", missing);
		printf("         (build them first: LADDER, then INSTALL)\n");
		return 1;
	}

	// Pass two: write it.
	files = 0;
	bytes = 0;
	dir_len = 0;
	at = 0;
	while ((at = next_entry(at, line, &kind)) >= 0) {
		if (kind && (kind != 2 || present(line))) {
			if ((n = copy_one(line)) < 0) {
				printf("install: could not write %s\n", line);
				return 1;
			}
			printf("  %s  %d\n", line, n);
			remember(line);
			++files;
			bytes = bytes + n;
		}
	}

	// What makes the medium bootable. A name rather than a second copy
	// of a 200 KB kernel called boot.c4r: the BIOS reads boot.cfg and
	// loads whatever it names (src/c4bb/fw/fw.c).
	if (bootname) {
		n = xlen(bootname);
		bootname[n] = 10;
		if (bb_put(at_dest("boot.cfg"), bootname, n + 1) < 0) {
			printf("install: could not write boot.cfg\n");
			return 1;
		}
		bootname[n] = 0;
		remember("boot.cfg");
		++files;
	}

	// And what makes it readable to a C4DOS, if one ever boots from
	// here: the raw medium cannot enumerate itself, so this file IS the
	// directory. Costs one write and saves the medium being a black box.
	remember("c4dos.dir");
	if (bb_put(at_dest("c4dos.dir"), dirbuf, dir_len) < 0) {
		printf("install: could not write c4dos.dir\n");
		return 1;
	}
	++files;

	printf("install: %d files, %d bytes onto drive %d", files, bytes, dest);
	if (bootname) printf(", boot.cfg -> %s", bootname);
	printf("\n");
	if (bootname)
		printf("install: eject drive %d and reboot to start it\n", keep);
	return 0;
}

// kinstall -- write a bootable medium, from inside C4KE.
//
//   kinstall 2:              read c4ix.lst, write drive 2
//   kinstall 2: other.lst    a different manifest
//
// One rung up from src/c4dos/install.c and the same job: b4ke builds
// C4IX into the ramfs, the ramfs dies with the kernel that owns it, and
// the next power-on has to find a medium it can boot. `save` will put
// the ramfs on a drive, but a ramfs is not a boot medium -- the C4IX
// userland was never in it. Those images are on the medium THIS system
// booted from, which is the one about to be ejected.
//
// So, exactly as one rung down: a manifest of what belongs on the
// medium, and each name fetched from wherever it actually is. The
// ramfs first, then the drive -- so what b4ke just built shadows
// anything of the same name that shipped, which is the precedence a
// build wants. (It is also tar.c's `slurp`, for the same reason.)
//
// It is `kinstall` and not `install` because the C4DOS transient of
// that name is already on the climb disk and cannot run here -- it
// talks to a DOS that no longer exists. Two rungs, two programs, two
// names, the way bbsave and save are.
//
// Board only: it drives c4bb's disk registers (include/c4bb.h).
// docs/c4bb-storage.md M12.
#include <u0.h>
#include "c4bb.h"

enum {
	CHUNK    = 65536,
	LINE_MAX = 256,
	MAN_MAX  = 65536,
	NAME_MAX = 128,
	DIR_MAX  = 16384
};

static char *man;         // the manifest text
static int   man_len;
static char *buf;         // the streaming buffer, for files off the drive
static char *dst;         // "N:name", rebuilt per file
static char *bootname;
static char *dirbuf;      // names written, for the c4dos.dir we leave
static int   dir_len;
static int   dest;

static int xlen (char *s) { int n; n = 0; while (s[n]) ++n; return n; }

static void xcopy (char *d, char *s) { while (*s) { *d = *s; ++d; ++s; } *d = 0; }

// "1:" or "B:" -> the drive number, or -1. save.c's, verbatim.
static int drivenum (char *s) {
	int c;
	if (!s[0] || s[1] != ':' || s[2]) return 0 - 1;
	c = s[0];
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'A' && c <= 'Z') return c - 'A';
	if (c >= 'a' && c <= 'z') return c - 'a';
	return 0 - 1;
}

// Name a file on the destination drive. Prefixed rather than selected,
// because reads have to keep coming from the drive we booted off while
// the writes go somewhere else.
static char *at_dest (char *name) {
	dst[0] = '0' + dest;
	dst[1] = ':';
	xcopy(dst + 2, name);
	return dst;
}

// Copy one file. Two paths, because the two places a file can be are
// genuinely different: the ramfs hands back a pointer to the whole
// thing, and the drive has to be read in pieces.
static int copy_one (char *name) {
	int fd, in, n, len, total;
	char *p;

	if ((p = vfs_get(name, &len))) {
		if ((fd = bb_create(at_dest(name))) < 0) return 0 - 1;
		if (len > 0 && bb_write(fd, p, len) != len) { bb_close(fd); return 0 - 1; }
		if (bb_close(fd) < 0) return 0 - 1;
		return len;
	}
	if ((in = open(name, 0)) < 0) return 0 - 1;
	if ((fd = bb_create(at_dest(name))) < 0) { close(in); return 0 - 1; }
	total = 0;
	n = 1;
	while (n > 0) {
		if ((n = read(in, buf, CHUNK)) > 0) {
			if (bb_write(fd, buf, n) != n) { close(in); bb_close(fd); return 0 - 1; }
			total = total + n;
		}
	}
	close(in);
	if (bb_close(fd) < 0) return 0 - 1;
	return total;
}

static int present (char *name) {
	int in, len;
	if (vfs_get(name, &len)) return 1;
	if ((in = open(name, 0)) < 0) return 0;
	close(in);
	return 1;
}

static void remember (char *name) {
	int n;
	n = xlen(name);
	if (dir_len + n + 1 >= DIR_MAX) return;
	xcopy(dirbuf + dir_len, name);
	dir_len = dir_len + n;
	dirbuf[dir_len] = 10;
	++dir_len;
}

// One line of the manifest into `out`, and what kind it was:
// 0 nothing here, 1 required, 2 optional, 3 the boot image.
static int next_entry (int at, char *out, int *kind) {
	int n;

	*kind = 0;
	if (at >= man_len) return 0 - 1;
	n = 0;
	while (at < man_len && man[at] != 10) {
		if (n < LINE_MAX - 1) { out[n] = man[at]; ++n; }
		++at;
	}
	if (at < man_len) ++at;
	out[n] = 0;
	n = 0;
	while (out[n] == ' ' || out[n] == 9) ++n;
	if (n) xcopy(out, out + n);
	n = xlen(out);
	while (n && (out[n - 1] == ' ' || out[n - 1] == 9 || out[n - 1] == 13)) {
		--n;
		out[n] = 0;
	}
	if (!out[0] || out[0] == '#') return at;

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

// The manifest itself comes from wherever it is, same rule as
// everything else it names.
static int read_manifest (char *name) {
	int in, n, len;
	char *p;

	if ((p = vfs_get(name, &len))) {
		if (len > MAN_MAX) len = MAN_MAX;
		memcpy(man, p, len);
		return len;
	}
	if ((in = open(name, 0)) < 0) return 0 - 1;
	man_len = 0;
	n = 1;
	while (n > 0 && man_len < MAN_MAX) {
		if ((n = read(in, man + man_len, 4096)) > 0) man_len = man_len + n;
	}
	close(in);
	return man_len;
}

int main (int argc, char **argv) {
	char *lst, *line;
	int at, kind, keep, missing, n, files, bytes;

	if (argc < 2) {
		printf("usage: kinstall <drive>: [manifest]\n");
		printf("  writes a bootable medium; default manifest is c4ix.lst\n");
		return 1;
	}
	lst = "c4ix.lst";
	if (argc > 2) lst = argv[2];

	if ((dest = drivenum(argv[1])) < 0) {
		printf("kinstall: '%s' is not a drive -- try 2: or C:\n", argv[1]);
		return 1;
	}
	if (dest > 9) {
		printf("kinstall: drive %d is past the last one this can name\n", dest);
		return 1;
	}
	if (dest >= bb_drives()) {
		printf("kinstall: this machine has %d drive(s), no %d\n", bb_drives(), dest);
		return 1;
	}
	keep = bb_drive();
	if (dest == keep) {
		printf("kinstall: drive %d is the one we are reading from\n", dest);
		return 1;
	}
	bb_select(dest);
	n = bb_readonly();
	bb_select(keep);
	if (n) {
		printf("kinstall: drive %d is read-only -- nothing to write to\n", dest);
		return 1;
	}

	if (!(man = malloc(MAN_MAX)) || !(buf = malloc(CHUNK))
	    || !(dst = malloc(NAME_MAX + 4)) || !(line = malloc(LINE_MAX))
	    || !(dirbuf = malloc(DIR_MAX))) {
		printf("kinstall: out of memory\n");
		return 1;
	}
	if ((man_len = read_manifest(lst)) < 0) {
		printf("kinstall: cannot read the manifest '%s'\n", lst);
		return 1;
	}

	// Pass one: everything required has to be findable before anything
	// is written. A medium that is half an operating system boots, and
	// then fails much further in, where the reason is far harder to see.
	printf("kinstall: reading %s\n", lst);
	bootname = 0;
	missing = 0;
	at = 0;
	while ((at = next_entry(at, line, &kind)) >= 0) {
		if (kind == 3) {
			if (!(bootname = malloc(NAME_MAX))) {
				printf("kinstall: out of memory\n");
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
		printf("kinstall: %d file(s) not found -- nothing written\n", missing);
		printf("          (build it first: tar x c4ix-src.tar, then b4ke -f c4ix.b4k)\n");
		return 1;
	}

	// Pass two.
	files = 0;
	bytes = 0;
	dir_len = 0;
	at = 0;
	while ((at = next_entry(at, line, &kind)) >= 0) {
		if (kind && (kind != 2 || present(line))) {
			if ((n = copy_one(line)) < 0) {
				printf("kinstall: could not write %s\n", line);
				return 1;
			}
			printf("  %s  %d\n", line, n);
			remember(line);
			++files;
			bytes = bytes + n;
		}
	}

	if (bootname) {
		n = xlen(bootname);
		bootname[n] = 10;
		if (bb_put(at_dest("boot.cfg"), bootname, n + 1) < 0) {
			printf("kinstall: could not write boot.cfg\n");
			return 1;
		}
		bootname[n] = 0;
		remember("boot.cfg");
		++files;
	}
	remember("c4dos.dir");
	if (bb_put(at_dest("c4dos.dir"), dirbuf, dir_len) < 0) {
		printf("kinstall: could not write c4dos.dir\n");
		return 1;
	}
	++files;

	printf("kinstall: %d files, %d bytes onto drive %d", files, bytes, dest);
	if (bootname) printf(", boot.cfg -> %s", bootname);
	printf("\n");
	if (bootname)
		printf("kinstall: eject drive %d and reboot to start it\n", keep);
	return 0;
}

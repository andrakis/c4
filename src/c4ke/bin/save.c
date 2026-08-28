// save -- write C4KE's RAM filesystem onto a drive.
//
//   save 1:                   every ramfs file onto drive 1
//   save 1: c4ix.c4r          just that one
//
// The same job src/c4dos/bbsave.c does one rung down, and for the same
// reason: b4ke builds C4IX into the ramfs, and the ramfs dies with the
// kernel that owns it. Put c4ix.c4r on a medium and the next power-on
// can boot it.
//
// Board only -- it drives c4bb's disk registers directly
// (include/c4bb.h). docs/c4bb-storage.md.
#include <u0.h>
#include "c4bb.h"

static int nameeq (char *a, char *b) {
	int i;
	i = 0;
	while (a[i] && a[i] == b[i]) ++i;
	return a[i] == b[i];
}

// "1:" or "B:" -> the drive number, or -1.
static int drivenum (char *s) {
	int c;
	if (!s[0] || s[1] != ':' || s[2]) return 0 - 1;
	c = s[0];
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'A' && c <= 'Z') return c - 'A';
	if (c >= 'a' && c <= 'z') return c - 'a';
	return 0 - 1;
}

static int wanted (int argc, char **argv, char *name) {
	int i;
	if (argc <= 2) return 1;            // no list: everything
	i = 2;
	while (i < argc) { if (nameeq(argv[i], name)) return 1; ++i; }
	return 0;
}

int main (int argc, char **argv) {
	char *name, *data;
	int   drv, n, i, saved, bytes, len, keep;

	if (argc < 2) {
		printf("usage: save <drive>: [file...]\n");
		printf("  writes the RAM filesystem onto that drive; no files means all\n");
		return 1;
	}
	if ((drv = drivenum(argv[1])) < 0) {
		printf("save: '%s' is not a drive -- try 1: or B:\n", argv[1]);
		return 1;
	}
	if (drv >= bb_drives()) {
		printf("save: this machine has %d drive(s), no %d\n", bb_drives(), drv);
		return 1;
	}
	keep = bb_drive();
	bb_select(drv);
	if (bb_readonly()) {
		printf("save: drive %d is read-only -- nothing to write to\n", drv);
		bb_select(keep);
		return 1;
	}

	n = vfs_count();
	i = 0;
	saved = 0;
	bytes = 0;
	while (i < n) {
		if ((name = vfs_name(i)) && (data = vfs_get(name, &len))) {
			if (wanted(argc, argv, name)) {
				if (bb_put(name, data, len) < 0) {
					printf("save: could not write %s\n", name);
					bb_select(keep);
					return 1;
				}
				printf("  %s  %d\n", name, len);
				++saved;
				bytes = bytes + len;
			}
		}
		++i;
	}
	bb_select(keep);
	printf("save: %d files, %d bytes onto drive %d\n", saved, bytes, drv);
	return 0;
}

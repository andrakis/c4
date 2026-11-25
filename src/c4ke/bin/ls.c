// C4 Ls
//
// Lists the kernel's RAM filesystem (OP_VFS_*), populated at boot by
// vfsload from c4ke.vfs.txt (see that file and vfsload.c).
//
// ramfs is a FLAT namespace: every entry is really a full path, and the
// first version of this printed all of them, which on the 151-entry
// root disk is four screens and no way to tell a program from a header.
// So the directory structure is inferred from the names rather than
// stored: everything under the given prefix is split at the next '/',
// files are listed by their remaining name and the first component of
// anything deeper is listed once, with a trailing '/'. One level, like
// ls has always meant.
//
//   ls           what is at the top level -- both the absolute names
//                vfsload loaded and the bare ones tools have written
//   ls /bin      the programs
//   ls -a        every entry, full paths, the old behaviour
//
#include <stdio.h>

static int vlen (char *s) { char *t; t = s; while (*t) ++t; return t - s; }

static int starts_with (char *s, char *pfx) {
	while (*pfx) { if (*s != *pfx) return 0; ++s; ++pfx; }
	return 1;
}

static int nameeq (char *a, char *b) {
	while (*a && *a == *b) { ++a; ++b; }
	return *a == *b;
}

// Simple insertion sort: vfs_count() is capped at RAMFS_MAX (256), so
// O(n^2) is fine and needs no scratch allocation beyond the pointers.
static void sort_names (char **names, int n) {
	int i, j;
	char *key, *a, *b;
	i = 1;
	while (i < n) {
		key = names[i];
		j = i - 1;
		while (j >= 0) {
			a = names[j]; b = key;
			while (*a && *a == *b) { ++a; ++b; }
			if (*a <= *b) break;
			names[j + 1] = names[j];
			--j;
		}
		names[j + 1] = key;
		++i;
	}
}

// One entry of the listing. Directories are synthesised, so they need
// somewhere to live that is not the ramfs name itself.
char **ent_name;
int   *ent_dir;
int    ent_n;

static int add_entry (char *name, int isdir) {
	int i;
	i = 0;
	while (i < ent_n) {
		if (ent_dir[i] == isdir && nameeq(ent_name[i], name)) return 0;
		++i;
	}
	ent_name[ent_n] = name;
	ent_dir[ent_n] = isdir;
	++ent_n;
	return 1;
}

char *dirbuf;
int   dirused;

// Copy the first path component out of `rest`, with a '/' on the end.
// The names we are given belong to the kernel; a synthesised directory
// name has to be ours.
static char *make_dir (char *rest, int len) {
	char *d;
	int   i;
	d = dirbuf + dirused;
	i = 0;
	while (i < len) { d[i] = rest[i]; ++i; }
	d[i] = '/';
	d[i + 1] = 0;
	dirused = dirused + len + 2;
	return d;
}

int main (int argc, char **argv) {
	char **names;
	char  *prefix, *name, *rest, *arg;
	int    total, i, n, col, maxw, w, cols, all, plen, cut;

	total = vfs_count();
	if (!total) {
		printf("(filesystem empty - vfsload has not populated it yet)\n");
		return 0;
	}

	all = 0;
	arg = "";
	i = 1;
	while (i < argc) {
		if (argv[i][0] == '-' && argv[i][1] == 'a') all = 1;
		else arg = argv[i];
		++i;
	}

	// "/bin" and "/bin/" mean the same thing; "" and "/" are the root.
	prefix = malloc(vlen(arg) + 2);
	i = 0;
	while (arg[i]) { prefix[i] = arg[i]; ++i; }
	if (i && prefix[i - 1] != '/') prefix[i++] = '/';
	prefix[i] = 0;
	plen = i;

	if (!(names   = malloc(total * sizeof(int))) ||
	    !(ent_name = malloc(total * sizeof(int))) ||
	    !(ent_dir  = malloc(total * sizeof(int)))) {
		printf("ls: out of memory\n"); return 1;
	}
	// Worst case every entry is a distinct directory name, and a name
	// is at most as long as the whole path.
	if (!(dirbuf = malloc(total * 258))) { printf("ls: out of memory\n"); return 1; }
	dirused = 0;
	ent_n = 0;

	n = 0;
	i = 0;
	while (i < total) {
		if ((name = vfs_name(i)) && starts_with(name, prefix)) {
			if (all) { names[n++] = name; }
			else {
				rest = name + plen;
				if (!*rest) { ++i; continue; }      // the directory itself
				// Not every name is absolute: vfsload's are, and
				// anything a tool wrote into the ramfs (c4rlink's
				// c4ix.c4r, b4ke's objects) is a bare name. With no
				// prefix at all, step over the leading slash so the
				// root lists "bin/" rather than "/".
				if (!plen && *rest == '/') ++rest;
				cut = 0;
				while (rest[cut] && rest[cut] != '/') ++cut;
				if (rest[cut] == '/') add_entry(make_dir(rest, cut), 1);
				else                  add_entry(rest, 0);
			}
		}
		++i;
	}
	if (!all) { i = 0; while (i < ent_n) { names[i] = ent_name[i]; ++i; } n = ent_n; }

	if (!n) {
		printf("ls: nothing under '%s'\n", plen ? prefix : "/");
		return 0;
	}

	sort_names(names, n);
	maxw = 0;
	i = 0;
	while (i < n) { if ((w = vlen(names[i])) > maxw) maxw = w; ++i; }

	cols = 80 / (maxw + 2);
	if (cols < 1) cols = 1;
	col = 0;
	i = 0;
	while (i < n) {
		printf("%-*s", maxw + 2, names[i]);
		if (++col >= cols) { printf("\n"); col = 0; }
		++i;
	}
	if (col) printf("\n");

	printf("%d entr%s in %s\n", n, n == 1 ? "y" : "ies", plen ? prefix : "/");
	return 0;
}

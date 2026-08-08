// C4 Ls
//
// Lists the kernel's RAM filesystem (OP_VFS_*), populated at boot by
// vfsload from c4ke.vfs.txt (see that file and vfsload.c). Since
// ramfs is a flat namespace, every entry is really a full path (e.g.
// "/bin/ls", "/usr/src/bin/ls.c") - an optional argument filters to
// entries starting with that prefix, giving basic directory-like
// browsing without ramfs needing to know what a directory is.

#include <stdio.h>

static int vlen (char *s) { char *t; t = s; while (*t) ++t; return t - s; }

static int starts_with (char *s, char *pfx) {
	while (*pfx) { if (*s != *pfx) return 0; ++s; ++pfx; }
	return 1;
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

int main (int argc, char **argv) {
	int total, n, i, col, maxw, w, cols;
	char **names, *prefix, *name;

	total = vfs_count();
	if (!total) {
		printf("(filesystem empty - vfsload has not populated it yet)\n");
		return 0;
	}

	prefix = argc > 1 ? argv[1] : "";
	if (!(names = malloc(total * sizeof(int)))) { printf("ls: out of memory\n"); return 1; }

	n = 0; maxw = 0;
	i = 0;
	while (i < total) {
		if ((name = vfs_name(i)) && starts_with(name, prefix)) {
			names[n++] = name;
			if ((w = vlen(name)) > maxw) maxw = w;
		}
		++i;
	}

	if (!n) {
		printf("ls: nothing under '%s'\n", prefix);
		free(names);
		return 0;
	}

	sort_names(names, n);

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

	printf("%d entr%s\n", n, n == 1 ? "y" : "ies");
	free(names);
	return 0;
}

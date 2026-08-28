// tar -- unpack an archive into C4KE's RAM filesystem.
//
//   tar x archive.tar     extract every member
//   tar t archive.tar     list members, extract nothing
//
// dostar (src/c4dos/dostar.c) does this one rung down, onto the C4DOS
// RAM disk, and the format and the parser are deliberately the same.
// What differs is where the bytes land: C4DOS has dos_put, C4KE has
// vfs_put, and a kernel that can unpack its own source is a kernel that
// can build the next system without the one below it staging anything.
// That is the whole point of the rung -- C4DOS builds C4KE, and then
// C4KE, on its own, extracts C4IX's source and builds C4IX.
//
// The format, from dostar.c:
//
//   C4TAR1\n
//   <name> <decimal length>\n
//   <exactly that many raw bytes>
//   ... repeated ...
//
// The archive is looked for in the RAM filesystem first and on the disk
// second, so an archive that was itself unpacked from another one still
// works.
#include <u0.h>

enum { TAR_CHUNK = 65536 };

char *g_buf;
int   g_len, g_cap;

// The archive can be any size and there is no stat(), so read it in
// chunks and double the buffer when it fills. realloc is deliberately
// not used: it is the one allocator call this system has historically
// had trouble with (docs/task-memory.md), and malloc/copy/free is three
// lines.
static int grow () {
	char *n;
	int   i;
	if (!(n = malloc(g_cap + g_cap))) return 0;
	i = 0;
	while (i < g_len) { n[i] = g_buf[i]; ++i; }
	free(g_buf);
	g_buf = n;
	g_cap = g_cap + g_cap;
	return 1;
}

static int slurp (char *path) {
	char *p;
	int   fd, n, len;

	// The RAM filesystem first, same as the loader does.
	if ((p = vfs_get(path, &len))) {
		if (!(g_buf = malloc(len + 1))) return 0;
		n = 0;
		while (n < len) { g_buf[n] = p[n]; ++n; }
		g_len = len;
		g_cap = len + 1;
		return 1;
	}
	if ((fd = open(path, 0)) < 0) return 0;
	g_cap = TAR_CHUNK + TAR_CHUNK;
	if (!(g_buf = malloc(g_cap))) { close(fd); return 0; }
	g_len = 0;
	n = 1;
	while (n > 0) {
		if (g_len + TAR_CHUNK > g_cap) {
			if (!grow()) { close(fd); return 0; }
		}
		if ((n = read(fd, g_buf + g_len, TAR_CHUNK)) > 0) g_len = g_len + n;
	}
	close(fd);
	return 1;
}

// Walk one entry: p points at a header line. Returns the payload, or 0
// at the end. Fills g_name / g_size.
char *g_name;
int   g_size;

static char *entry (char *p, char *end) {
	char *q;
	int   n;
	if (p >= end) return 0;
	q = p;
	while (q < end && *q != ' ' && *q != 10) ++q;
	if (q >= end) return 0;
	n = q - p;
	if (n > 250) return 0;
	g_name = p;
	*q = 0;                       // the archive is scratch: terminate in place
	++q;
	g_size = 0;
	while (q < end && *q >= '0' && *q <= '9') {
		g_size = g_size * 10 + (*q - '0');
		++q;
	}
	while (q < end && *q != 10) ++q;
	if (q >= end) return 0;
	++q;                          // past the newline: payload starts here
	if (q + g_size > end) return 0;
	return q;
}

int main (int argc, char **argv) {
	char *p, *end, *data, *mode;
	int   extract, count, total;

	if (argc < 3) {
		printf("usage: tar x|t archive.tar\n");
		printf("  x  extract every member into the RAM filesystem\n");
		printf("  t  list members\n");
		return 1;
	}
	mode = argv[1];
	extract = (*mode == 'x' || *mode == 'X');

	if (!slurp(argv[2])) { printf("tar: cannot read %s\n", argv[2]); return 1; }
	if (g_len < 8) { printf("tar: %s is empty\n", argv[2]); return 1; }

	p = g_buf;
	end = g_buf + g_len;
	if (!(p[0] == 'C' && p[1] == '4' && p[2] == 'T' && p[3] == 'A' &&
	      p[4] == 'R' && p[5] == '1')) {
		printf("tar: %s is not a C4TAR1 archive\n", argv[2]);
		return 1;
	}
	while (p < end && *p != 10) ++p;
	if (p < end) ++p;

	count = 0;
	total = 0;
	while (p < end) {
		if (!(data = entry(p, end))) p = end;
		else {
			if (extract) {
				if (vfs_put(g_name, data, g_size) < 0) {
					printf("tar: could not write %s\n", g_name);
					return 1;
				}
			} else {
				printf("%s  %d\n", g_name, g_size);
			}
			++count;
			total = total + g_size;
			p = data + g_size;
		}
	}
	if (extract) printf("tar: extracted %d files, %d bytes\n", count, total);
	else         printf("tar: %d files, %d bytes\n", count, total);
	return 0;
}

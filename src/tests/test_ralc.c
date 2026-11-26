// test_ralc.c -- realloc in a protected task: what it held, it still holds.
//
// Under C4IX a C4KE program's malloc, free and realloc trap to the kernel
// (sys.c's sys_pmviolation). Until 2026-11-27 realloc, opcode 41, had no
// case there and the task was killed at its first one. This grows a block
// past a page and past the MMU's small-block slabs (UR_SMALL, 2048), shrinks
// it again, and starts one from a null pointer, checking every byte that
// should have come along.
#include <u0.h>

static int fill (char *p, int n, int seed) {
	int i;
	i = 0;
	while (i < n) { p[i] = (char)((i * 7 + seed) & 0x7f); ++i; }
	return n;
}

static int same (char *p, int n, int seed) {
	int i;
	i = 0;
	while (i < n) {
		if (p[i] != (char)((i * 7 + seed) & 0x7f)) {
			printf("test_ralc: byte %d is %d\n", i, p[i]);
			return 0;
		}
		++i;
	}
	return 1;
}

int main (int argc, char **argv) {
	char *p, *q;

	if (!(p = malloc(40))) { printf("test_ralc: no malloc\n"); return 1; }
	fill(p, 40, 3);
	// small to small, then past a slab, then past a page
	if (!(p = realloc(p, 100)) || !same(p, 40, 3)) { printf("test_ralc: 40 -> 100\n"); return 1; }
	fill(p, 100, 5);
	if (!(p = realloc(p, 3000)) || !same(p, 100, 5)) { printf("test_ralc: 100 -> 3000\n"); return 1; }
	fill(p, 3000, 9);
	if (!(p = realloc(p, 20000)) || !same(p, 3000, 9)) { printf("test_ralc: 3000 -> 20000\n"); return 1; }
	fill(p, 20000, 11);
	// and back down: what fits comes along
	if (!(p = realloc(p, 64)) || !same(p, 64, 11)) { printf("test_ralc: 20000 -> 64\n"); return 1; }
	// realloc(0, n) is malloc(n)
	if (!(q = realloc(0, 500))) { printf("test_ralc: realloc(0, 500)\n"); return 1; }
	fill(q, 500, 13);
	if (!same(q, 500, 13) || !same(p, 64, 11)) { printf("test_ralc: two blocks\n"); return 1; }
	free(q);
	free(p);
	printf("test_ralc: ok\n");
	return 0;
}

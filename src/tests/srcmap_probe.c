// srcmap_probe.c -- test-srcmap's probe (docs/c4-explorer.md, S1).
// Compiled with -g its map is pinned (src/c4sp/tests/expected/
// srcmap-probe.txt): rows for the header's function at the header's
// lines, a macro's code at the line that uses it, a for whose step is
// the for's own line, an inlined call, and __LINE__ counted past the
// backslash-newline in the header's macro and the one below.
#include "srcmap_probe.h"

#define SUM3(a, b, c) \
	((a) + (b) + (c))

int total;

int add(int a, int b) { return a + b; }

int main()
{
	int i;
	int j;
	total = 0;
	for (i = 0; i < 4; i = i + 1)
		total = total + square(i);
	j = TWICE(total);
	while (j > 100)
		j = j - SUM3(1, 2, 3);
	total = add(total, j) + fact(4);
	printf("%d %d %d\n", total, j, __LINE__);
	return 0;
}

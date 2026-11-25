// c4lc_m4std.c -- M4 types survive the standard headers.
//
// include/stddef.h #defines `unsigned` to nothing and `long` to int for
// c4cc, and stdio.h and stdlib.h include it. c4lc and c4fc predefine
// __c4_inttypes__, which turns those two defines off: every line below
// prints something else if `unsigned` quietly becomes int again (the
// compares, the division, the shifts) or `unsigned long` becomes int.
//
// The golden, src/c4sp/tests/expected/c4lc-m4std.txt, is plain gcc's on
// this file. Nothing printed depends on the word size. c4fc's subset: no
// typedef, no compound assignment, declarations at the top of a block.
//
// -D STDDEF_ONLY includes stddef.h alone (printf and malloc are builtins
// without the headers): c4lc's own preprocessor cannot read stdarg.h,
// which stdio.h pulls in, so that is how test-c4lc holds c4lc -P to it.

#ifdef STDDEF_ONLY
#include <stddef.h>
#else
#include <stdio.h>
#include <stdlib.h>
#endif

unsigned crc32(char *s, int n) {
	unsigned c;
	int i;
	int k;
	c = 0xFFFFFFFFu;
	i = 0;
	while (i < n) {
		c = c ^ (s[i] & 255);
		k = 0;
		while (k < 8) {
			if (c & 1) c = (c >> 1) ^ 0xEDB88320u;
			else c = c >> 1;
			k++;
		}
		i++;
	}
	return c ^ 0xFFFFFFFFu;
}

int main() {
	unsigned u;
	unsigned int ui;
	unsigned long ul;
	long l;
	char *p;

	u = 0; u = u - 1;
	printf("0-1 %u\n", u);
	printf("cmp %d %d\n", u > 1, u < 1);
	u = 4000000000u;
	printf("div %u mod %u\n", u / 3, u % 7);
	printf("shr %u\n", u >> 28);
	ui = 0x80000000u;
	printf("top %u %d\n", ui >> 31, ui > 0x7FFFFFFFu);
	ui = 4294967295u; ui = ui + 1;
	printf("wrap %u\n", ui);

	// unsigned long is word-sized unsigned, not int
	ul = 0; ul = ul - 1;
	printf("ul %d %d %d\n", ul > 0, (ul >> 1) > 0, ul / 2 > 1000000);
	l = -6;
	printf("long %ld %ld\n", l / 4, l >> 1);

	// stdlib.h is in the picture too: malloc through it
	p = malloc(16);
	p[0] = 'o'; p[1] = 'k'; p[2] = 0;
	printf("crc %u %s\n", crc32("123456789", 9), p);
	free(p);
	return 0;
}

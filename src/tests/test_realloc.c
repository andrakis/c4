// C4 Test: realloc(), and that malloc()/free() still behave alongside it.
//
// Compiled by gcc as the oracle: output must match exactly. c4cc compiles
// realloc() to the RALC opcode, which c4m did not implement until Track 0
// of docs/compiler-speed.md -- before that this test dies with an illegal
// instruction, which is the bug it exists to pin.
//
// Nothing here prints a pointer value: those differ between hosts by
// design. Only content, lengths and null-ness are compared.
#include <stdio.h>
#include <stdlib.h>

// Fill n bytes of p with a repeating a..z pattern seeded by k, so that a
// later check can tell "preserved" from "reallocated but not copied".
void fill (char *p, int n, int k) {
	int i;
	i = 0;
	while (i < n) { p[i] = 'a' + ((i + k) % 26); ++i; }
}

// Return 1 if the first n bytes of p still hold pattern k.
int check (char *p, int n, int k) {
	int i;
	i = 0;
	while (i < n) {
		if (p[i] != 'a' + ((i + k) % 26)) return 0;
		++i;
	}
	return 1;
}

int main (int argc, char **argv) {
	char *p;
	char *q;
	int   i, n, ok;

	// 1. plain malloc still works
	p = malloc(16);
	if (!p) { printf("malloc(16) failed\n"); return 1; }
	fill(p, 16, 0);
	printf("1 malloc16 %d\n", check(p, 16, 0));

	// 2. grow: the first 16 bytes must survive
	p = realloc(p, 64);
	if (!p) { printf("realloc grow failed\n"); return 1; }
	printf("2 grow preserved %d\n", check(p, 16, 0));
	fill(p, 64, 0);
	printf("2 grow usable %d\n", check(p, 64, 0));

	// 3. shrink: the first 8 bytes must survive
	p = realloc(p, 8);
	if (!p) { printf("realloc shrink failed\n"); return 1; }
	printf("3 shrink preserved %d\n", check(p, 8, 0));

	// 4. realloc(0, n) behaves as malloc(n)
	q = realloc(0, 32);
	if (!q) { printf("realloc(0,32) failed\n"); return 1; }
	fill(q, 32, 5);
	printf("4 fromnull %d\n", check(q, 32, 5));
	free(q);

	// 5. repeated growth, checking content at every step. This is the
	//    path that actually exercises copy-min(old,new).
	n  = 4;
	q  = malloc(n);
	fill(q, n, 1);
	ok = 1;
	i  = 0;
	while (i < 8) {
		q = realloc(q, n * 2);
		if (!q) { printf("realloc loop failed at %d\n", n); return 1; }
		if (!check(q, n, 1)) ok = 0;
		fill(q, n * 2, 1);
		n = n * 2;
		++i;
	}
	printf("5 growloop %d size %d\n", ok, n);
	free(q);

	// 6. free() after realloc() must not corrupt the heap: allocate and
	//    release repeatedly, then prove the allocator still works.
	i = 0;
	while (i < 64) {
		q = malloc(24);
		q = realloc(q, 96);
		fill(q, 96, i);
		if (!check(q, 96, i)) { printf("6 churn bad at %d\n", i); return 1; }
		free(q);
		++i;
	}
	q = malloc(40);
	fill(q, 40, 9);
	printf("6 churn %d\n", check(q, 40, 9));
	free(q);

	free(p);
	printf("done\n");
	return 0;
}

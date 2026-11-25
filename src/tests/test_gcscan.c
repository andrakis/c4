// Spike: can C4 code find and conservatively scan its own stack?
//
// This is the load-bearing assumption behind c4sp's garbage collector. If a
// collector can locate every cell pointer sitting in a C4 local variable, it
// can use mark-and-sweep without any GCPRO/root-push discipline in the
// interpreter.
//
// Facts used:
//   - The C4 stack grows downwards.
//   - Inside a function, &<first local> == bp - 1, so &<first local> + 1 == bp.
//   - Everything between the innermost frame and the recorded base of the
//     stack is live stack belonging to this call chain.
//
// Runs under c4m and under plain c4. 'static' is stripped for the plain-c4
// run because c4.c has no such keyword:
//   ./c4m src/tests/test_gcscan.c
//   sed 's/^static //' src/tests/test_gcscan.c > /tmp/t.c && ./c4 /tmp/t.c

#include <stdio.h>
#include <stdlib.h>

static int *stack_base;     // recorded once, in main
static int *arena;          // the pretend cell heap
static int  arena_words;
static int  found;

// Record where the stack starts. Called from main so its frame is the
// outermost one we care about.
static void gc_record_base (int *bp_of_main) {
	stack_base = bp_of_main;
}

// Is this word a plausible pointer into the arena, correctly aligned to a
// cell boundary? Two compares and a modulo -- cheap enough to run over the
// whole stack.
static int looks_like_cell (int w) {
	int off;
	if (w < (int)arena) return 0;
	if (w >= (int)(arena + arena_words)) return 0;
	off = w - (int)arena;
	if (off % (sizeof(int) * 4)) return 0;   // cells are 4 words
	return 1;
}

// Conservatively scan the stack for anything that looks like a cell pointer.
static int gc_scan_stack () {
	int *p;         // first local: &p + 1 == bp of this frame
	int *top;
	int n;

	// Start just below this frame and walk up to the recorded base.
	top = (int *)(&p + 1);
	n = 0;
	p = top;
	while (p < stack_base) {
		if (looks_like_cell(*p)) {
			printf("  found cell pointer 0x%lx at stack slot 0x%lx (depth %ld)\n",
			       *p, p, stack_base - p);
			++n;
		}
		++p;
	}
	return n;
}

// A few nested frames, each holding a pointer into the arena in a local.
// A correct scan must find all of them.
static int level3 (int *c) {
	int *mine;
	mine = c;
	printf("level3 holds 0x%lx\n", mine);
	return gc_scan_stack();
}

static int level2 (int *b) {
	int *mine;
	int r;
	mine = b;
	printf("level2 holds 0x%lx\n", mine);
	r = level3(arena + 8);
	return r;
}

static int level1 (int *a) {
	int *mine;
	int r;
	mine = a;
	printf("level1 holds 0x%lx\n", mine);
	r = level2(arena + 4);
	return r;
}

int main () {
	int base_marker;
	int n, expect;

	// &base_marker + 1 is this frame's bp: the top of the region we scan.
	gc_record_base((int *)(&base_marker + 1));

	arena_words = 64;
	arena = malloc(sizeof(int) * arena_words);
	if (!arena) { printf("no arena\n"); return 1; }
	memset(arena, 0, sizeof(int) * arena_words);

	printf("stack base 0x%lx, arena 0x%lx..0x%lx\n",
	       stack_base, arena, arena + arena_words);

	// Three nested frames each hold one pointer, and each call also leaves
	// the argument on the stack, so we expect to find at least 3.
	n = level1(arena + 0);

	expect = 3;
	printf("\nscan found %d cell-looking words (expected at least %d)\n", n, expect);
	if (n >= expect) printf("PASS: stack scanning works\n");
	else printf("FAIL: scan missed live references\n");

	free(arena);
	return n >= expect ? 0 : 1;
}

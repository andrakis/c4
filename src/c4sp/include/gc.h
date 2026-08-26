// c4sp: arena and allocator
//
// A LIST OF BLOCKS, each one malloc of NCELLS * CELL__Sz words. Cell
// references are real pointers into them. Swept cells are threaded through
// CELL_A to form one free list across all blocks, so allocation is still a
// pop: O(1).
//
// It was one block for a long time, and -c had to be guessed high enough
// for the largest thing a rule would ever compile -- which meant a
// half-gigabyte resident set for a job that needed eighteen megabytes, and
// a build that spent more time faulting in untouched arena than compiling.
// Guess too low instead and the collector thrashes: a 99%-live arena is
// collected once per allocation. The arena grows now, so -c is where it
// STARTS rather than what it is allowed to reach, and neither mistake is
// available any more.
//
// The collector (mark & sweep, conservative over the C4 stack) arrives at
// M2; until then gc_collect() reports arena exhaustion and exits. The entry
// point and statistics exist now so nothing above this file changes shape
// when the real collector lands.

#if NATIVE
#include <setjmp.h>
#endif

// Blocks. Doubling keeps the list short, and the list is walked once per
// candidate pointer during the conservative scan, so short matters.
enum { GC_MAXBLOCKS = 24 };
int  gc_block[GC_MAXBLOCKS];        // base address of each block
int  gc_block_cells[GC_MAXBLOCKS];  // cells in it
int  gc_block_first[GC_MAXBLOCKS];  // index of its first cell in gc_marks
int  gc_nblocks;
int  gc_ncells;       // cells across every block
int *gc_freelist;     // head of the free list (threaded through CELL_A)
int  gc_free_count;   // cells currently on the free list
int *gc_stack_base;   // recorded once at startup, scan upper bound (M2)

// Statistics, reported by (gc:stats)
int  gc_total_allocs; // cells handed out since start
int  gc_collections;  // collect() runs
int  gc_last_freed;   // cells reclaimed by the last collect
int  gc_last_live;    // cells found live by the last collect

// Mark state
char *gc_marks;       // one byte per cell
int  *gc_worklist;    // explicit mark worklist; a cell enters at most once,
                      // so gc_ncells entries can never overflow
int   gc_wl_top;

// Explicit roots. The global environment reaches almost everything; the
// spare slots are for the CEK machine's registers (design 6.2), so the
// collector needs no changes when that lands.
int *gc_root_genv;
int *gc_root_true, *gc_root_false;
int *gc_root_a, *gc_root_b, *gc_root_c;

// Add a block, thread its cells onto the free list, and resize the mark
// state. Returns 0 on success. The mark arrays carry nothing between
// collections, so they are simply rebuilt rather than copied.
int gc_addblock (int n) {
	int *c, *base;
	int  i;

	if (gc_nblocks >= GC_MAXBLOCKS) return 1;
	if (n < 256) n = 256;
	if (!(base = malloc(n * CELL__Sz * sizeof(int)))) return 1;
	memset(base, 0, n * CELL__Sz * sizeof(int));

	gc_block[gc_nblocks]       = (int)base;
	gc_block_cells[gc_nblocks] = n;
	gc_block_first[gc_nblocks] = gc_ncells;
	++gc_nblocks;

	// first cell on top, as the single-arena version threaded it
	i = n;
	while (i--) {
		c = base + i * CELL__Sz;
		c[CELL_A] = (int)gc_freelist;
		gc_freelist = c;
	}
	gc_ncells     = gc_ncells + n;
	gc_free_count = gc_free_count + n;

	if (gc_marks)    free(gc_marks);
	if (gc_worklist) free((char *)gc_worklist);
	gc_marks = 0;
	gc_worklist = 0;
	if (!(gc_marks = malloc(gc_ncells)) ||
	    !(gc_worklist = malloc(gc_ncells * sizeof(int)))) {
		printf("c4sp: cannot allocate gc mark state\n");
		return 1;
	}

	return 0;
}

// Initialize the arena and thread the free list. Returns 0 on success.
// NCELLS is the first block; the rest arrive as they are needed.
int gc_init (int ncells) {
	gc_nblocks = 0;
	gc_ncells = 0;
	gc_freelist = 0;
	gc_free_count = 0;
	gc_marks = 0;
	gc_worklist = 0;
	gc_total_allocs = gc_collections = gc_last_freed = gc_last_live = 0;
	if (gc_addblock(ncells)) {
		printf("c4sp: cannot allocate %d-cell arena\n", ncells);
		return 1;
	}
	return 0;
}

// Where w sits in the mark array, or -1 if it is not a cell pointer:
// inside some block and aligned to a cell boundary. Because the collector
// never moves anything, a wrong guess here can only retain a dead cell,
// never corrupt a live one.
int gc_cellidx (int w) {
	int b, off, sz;
	sz = sizeof(int) * CELL__Sz;
	b = 0;
	while (b < gc_nblocks) {
		off = w - gc_block[b];
		if (off >= 0 && off < gc_block_cells[b] * sz) {
			if (off % sz) return -1;
			return gc_block_first[b] + off / sz;
		}
		++b;
	}
	return -1;
}

int gc_valid_cell (int w) { return gc_cellidx(w) >= 0; }

// Mark one candidate cell and queue it for its children to be visited.
void gc_mark (int w) {
	int idx;
	idx = gc_cellidx(w);
	if (idx < 0) return;
	if (gc_marks[idx]) return;
	gc_marks[idx] = 1;
	gc_worklist[gc_wl_top++] = w;
}

// Visit queued cells' children. Typed, not conservative: only the slots
// that hold cell references per the table in cell.h are followed. A cell
// retained by a stale stack word is on the free list with type T_NIL, so
// it contributes nothing here.
void gc_drain () {
	int *c, t;
	while (gc_wl_top) {
		c = (int *)gc_worklist[--gc_wl_top];
		t = c[CELL_TYPE];
		if (t == T_CONS || t == T_ENV || t == T_CONT) {
			gc_mark(c[CELL_A]);
			gc_mark(c[CELL_B]);
		} else if (t == T_LAMBDA || t == T_MACRO || t == T_FASTMACRO ||
		           (t >= T_KARG && t <= T_KMACRO)) {
			// Closures and kont frames: every used slot is a cell or 0
			gc_mark(c[CELL_A]);
			gc_mark(c[CELL_B]);
			gc_mark(c[CELL_C]);
		}
	}
}

// Mark & sweep, conservative over the C4 stack (design 4). The stack scan
// runs from this frame's base pointer (&p + 1 == bp, the property verified
// by src/tests/test_gcscan.c) up to the base recorded in main, so every
// cell reference held in any live C4 local is found without any root
// registration discipline in the interpreter.
//
// NOTE (native builds): this relies on locals living in stack memory, so
// the native binary must be compiled -O0; an optimizing gcc may keep the
// only reference to a cell in a callee-saved register. Under the C4 VM the
// scan is exact by construction.
void gc_collect () {
#if NATIVE
	// Spill the callee-saved registers into a buffer that lives in this
	// frame, so the stack scan below sees them. Without this the scan
	// only finds cells that happen to be in memory, and an optimising
	// gcc is free to keep the ONLY reference to a live cell in a
	// register -- which is why this file forced the native build to
	// -O0 (Makefile). The buffer is scanned first because p starts at
	// its address. Under the C4 VM there are no such registers and the
	// scan is already exact, so this is native-only.
	jmp_buf gc_regs;
#endif
	int *p, *c, *base;
	int i, b, freed;

	++gc_collections;
	memset(gc_marks, 0, gc_ncells);
	gc_wl_top = 0;

	// Roots: the interpreter globals...
	gc_mark((int)gc_root_genv);
	gc_mark((int)gc_root_true);
	gc_mark((int)gc_root_false);
	gc_mark((int)gc_root_a);
	gc_mark((int)gc_root_b);
	gc_mark((int)gc_root_c);
	// ...and the C4 stack, scanned conservatively.
#if NATIVE
	setjmp(gc_regs);
	p = (int *)&gc_regs;
#else
	p = (int *)(&p + 1);
#endif
	while (p < gc_stack_base) {
		gc_mark(*p);
		++p;
	}
	gc_drain();

	// Sweep: rebuild the free list from unmarked cells. Dead strings give
	// their malloc'd buffer back; freeing is idempotent because the type
	// resets to T_NIL.
	gc_freelist = 0;
	gc_free_count = 0;
	freed = 0;
	gc_last_live = 0;
	b = gc_nblocks;
	while (b--) {
		base = (int *)gc_block[b];
		i = gc_block_cells[b];
		while (i--) {
			if (gc_marks[gc_block_first[b] + i]) ++gc_last_live;
			else {
				c = base + i * CELL__Sz;
				if (c[CELL_TYPE] == T_STRING && c[CELL_A])
					free((char *)c[CELL_A]);
				c[CELL_TYPE] = 0;
				c[CELL_B] = c[CELL_C] = 0;
				c[CELL_A] = (int)gc_freelist;
				gc_freelist = c;
				++gc_free_count;
				++freed;
			}
		}
	}
	gc_last_freed = freed;

	// Grow rather than thrash, and grow against the LIVE SET rather than
	// against the arena.
	//
	// What a collection costs is set by how much is LIVE -- marking
	// visits every reachable cell -- and what it returns is the free
	// space. So the total mark work over a run is (allocations / free
	// per cycle) * live, and the only lever is how much free space each
	// cycle leaves. A heap of twice the live data halves nothing: it
	// makes mark work equal total allocations. Four times makes it a
	// third of that, and a compiler's live set only ever grows, so the
	// memory is going to be wanted anyway.
	//
	// The block is sized to restore the ratio in ONE step, and to at
	// least double the arena, because arriving at the right size by
	// small steps means a full collection at every size on the way --
	// and because a block list that is walked per candidate pointer
	// wants to stay short.
	if (gc_free_count < gc_last_live * 3) {
		i = gc_last_live * 4 - gc_ncells;
		if (i < gc_ncells) i = gc_ncells;
		gc_addblock(i);
	}

	if (!gc_freelist) {
		printf("c4sp: arena exhausted: all %d cells are live\n", gc_ncells);
		exit(1);
	}
}

// Pop a zeroed cell off the free list.
int *gc_alloc_cell () {
	int *c;
	if (!gc_freelist) gc_collect();
	c = gc_freelist;
	gc_freelist = (int *)c[CELL_A];
	--gc_free_count;
	++gc_total_allocs;
	c[CELL_TYPE] = c[CELL_A] = c[CELL_B] = c[CELL_C] = 0;
	return c;
}

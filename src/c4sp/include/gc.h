// c4sp: arena and allocator
//
// One malloc of NCELLS * CELL__Sz words. Cell references are real pointers
// into it. Swept cells are threaded through CELL_A to form a free list, so
// allocation is a pop: O(1).
//
// The collector (mark & sweep, conservative over the C4 stack) arrives at
// M2; until then gc_collect() reports arena exhaustion and exits. The entry
// point and statistics exist now so nothing above this file changes shape
// when the real collector lands.

int *gc_arena;        // base of the cell arena
int  gc_arena_words;  // total words in the arena
int  gc_ncells;       // cells in the arena
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

// Initialize the arena and thread the free list. Returns 0 on success.
int gc_init (int ncells) {
	int *c, i;
	gc_ncells      = ncells;
	gc_arena_words = ncells * CELL__Sz;
	if (!(gc_arena = malloc(gc_arena_words * sizeof(int)))) {
		printf("c4sp: cannot allocate %d-cell arena\n", ncells);
		return 1;
	}
	memset(gc_arena, 0, gc_arena_words * sizeof(int));
	// Thread every cell onto the free list, first cell on top
	gc_freelist = 0;
	i = ncells;
	while (i--) {
		c = gc_arena + i * CELL__Sz;
		c[CELL_A] = (int)gc_freelist;
		gc_freelist = c;
	}
	gc_free_count = ncells;
	gc_total_allocs = gc_collections = gc_last_freed = gc_last_live = 0;
	if (!(gc_marks = malloc(ncells)) ||
	    !(gc_worklist = malloc(ncells * sizeof(int)))) {
		printf("c4sp: cannot allocate gc mark state\n");
		return 1;
	}
	return 0;
}

// Is w a plausible pointer to a cell: inside the arena and aligned to a
// cell boundary? Because the collector never moves anything, a wrong guess
// here can only retain a dead cell, never corrupt a live one.
int gc_valid_cell (int w) {
	int off;
	if (w < (int)gc_arena) return 0;
	if (w >= (int)(gc_arena + gc_arena_words)) return 0;
	off = w - (int)gc_arena;
	return off % (sizeof(int) * CELL__Sz) == 0;
}

// Mark one candidate cell and queue it for its children to be visited.
void gc_mark (int w) {
	int idx;
	if (!gc_valid_cell(w)) return;
	idx = (w - (int)gc_arena) / (sizeof(int) * CELL__Sz);
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
		if (t == T_CONS || t == T_ENV) {
			gc_mark(c[CELL_A]);
			gc_mark(c[CELL_B]);
		} else if (t == T_LAMBDA || t == T_MACRO || t == T_FASTMACRO) {
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
	int *p, *c;
	int i, freed;

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
	p = (int *)(&p + 1);
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
	i = gc_ncells;
	while (i--) {
		if (gc_marks[i]) ++gc_last_live;
		else {
			c = gc_arena + i * CELL__Sz;
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
	gc_last_freed = freed;

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

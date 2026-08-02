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
	return 0;
}

// M2 replaces this body with mark & sweep. Called when the free list runs
// dry; for M0/M1 that is fatal.
void gc_collect () {
	++gc_collections;
	if (!gc_freelist) {
		printf("c4sp: cell arena exhausted (%d cells) and no collector yet\n",
		       gc_ncells);
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

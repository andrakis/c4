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
// The collector is mark & sweep, conservative over the C4 stack (M2); the
// arena grows by adding blocks when a collection leaves too little free
// (docs/c4sp-design.md).

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

// Extra roots, for cells held ONLY in a C global.
//
// The conservative stack scan finds everything the interpreter holds,
// because the interpreter holds it in locals. Compiled code
// (docs/c4sc-design.md) does not: a generated unit keeps its literal
// table and its top-level defines in C globals, which are not on the
// stack and were not scanned. That is the one rooting job compilation
// adds, and it is a slot list rather than a value list because a global
// like a counter is REASSIGNED -- rooting the cell it held at startup
// would root the wrong thing after the first `set!`.
enum { GC_EXTRA_MAX = 4096 };
int  *gc_extra;       // each entry is the ADDRESS of an int * variable
int   gc_nextra;
int gc_add_root (int *slot) {
	if (gc_nextra >= GC_EXTRA_MAX) return 0;
	gc_extra[gc_nextra++] = (int)slot;
	return 1;
}

// Statistics, reported by (gc:stats)
int  gc_total_allocs; // cells handed out since start
int  gc_collections;  // collect() runs
int  gc_last_freed;   // cells reclaimed by the last collect
int  gc_last_live;    // cells found live by the last collect

// Mark state: a bit a cell, and a worklist of a fixed size.
//
// It was a byte a cell and a worklist entry a cell -- five bytes, against
// the cell's sixteen -- which could never overflow, and which was a
// quarter of a large compile's arena again: 6.7 MB for c4sc compiling
// the desktop's edit.c inside C4IX's 64 MB machine, where that was what
// ran out. A bit a cell is an eighth of a byte. The worklist holds only
// cells that have children to visit, and a car is walked while its cdr
// waits (gc_trace), so it holds about as many cells as data nests deep.
// When it is full all the same, a cell is marked but not queued,
// gc_wl_over says so, and gc_drain finds it again by walking the arena
// for marked cells.
enum { GC_WL_MAX = 16384 };
char *gc_marks;       // one bit per cell: cell k is bit k & 7 of byte k >> 3
int  *gc_worklist;    // GC_WL_MAX cells whose children are still to visit
int   gc_wl_top;
int   gc_wl_over;     // a marked cell did not fit on the worklist

// The global-environment index, defined here rather than in cells.h only
// because gc_shutdown below releases it and c4 wants a declaration first.
int *env_gidx;       // atom id -> (atom . value) pair, 0 = not bound
int  env_gidx_cap;
int  env_gidx_off;   // set once if an allocation fails: scan from then on

// Explicit roots. The global environment reaches almost everything; the
// spare slots are for the CEK machine's registers (design 6.2), so the
// collector needs no changes when that lands.
int *gc_root_genv;
int *gc_root_true, *gc_root_false;
int *gc_root_a, *gc_root_b, *gc_root_c;

// The mark bits for N cells, or 0 when there is not the memory for them.
int gc_marksfor (int n) {
	if (!(gc_marks = malloc((n + 7) / 8))) return 0;
	return 1;
}

// Add a block, thread its cells onto the free list, and resize the mark
// bits. Returns 0 on success. The bits carry nothing between collections,
// so they are simply rebuilt rather than copied.
//
// They are given back BEFORE the block is asked for, and made again
// after it. On a machine whose memory is all there is -- a C4IX task in a
// 64 MB machine -- holding the old ones across the block's malloc is
// what made the new ones fail, when the mark state was five bytes a cell
// (desktop/wm.c, compiled by c4sc inside C4IX's own rebuild).
//
// Without its mark state the collector cannot run at all, and it would
// mark through a null pointer -- with no MMU that is the kernel's own low
// memory, and the machine panics a moment later. So failing to make it
// is the end of the run, said plainly, not a return code.
int gc_addblock (int n) {
	int *c, *base;
	int  i;

	if (gc_nblocks >= GC_MAXBLOCKS) return 1;
	if (n < 256) n = 256;
	if (gc_marks) free(gc_marks);
	gc_marks = 0;
	// The block, and the mark state for the arena with it. Without room
	// for both the arena stays as it was -- with its own mark state, or
	// the run cannot go on -- and the caller may ask for less (gc_collect).
	base = (int *)malloc(n * CELL__Sz * sizeof(int));
	if (!base || !gc_marksfor(gc_ncells + n)) {
		if (base) free((char *)base);
		if (gc_ncells && !gc_marksfor(gc_ncells)) {
			printf("c4sp: cannot allocate gc mark state for %d cells\n", gc_ncells);
			exit(1);
		}
		return 1;
	}
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
	return 0;
}

// Give the arena back.
//
// A process exiting on a host does not need this -- the operating system
// reclaims everything, and for years that was the whole story. A TASK
// exiting under C4KE does: the kernel returns the stack, the code, the
// data, the argv and the C4R it allocated FOR the task, and has no way
// to know about anything the task allocated for ITSELF. Twelve compiler
// runs in one session then leave twelve arenas behind, and the arena is
// most of what a compiler run is. See docs/task-memory.md.
//
// Strings own a malloc'd buffer outside the arena, so they have to be
// released one at a time; a cell on the free list has type T_NIL (the
// sweep clears it), so the test is the same one the sweep uses.
void gc_shutdown () {
	int *base, *c;
	int  b, i;

	b = gc_nblocks;
	while (b--) {
		base = (int *)gc_block[b];
		if (!base) continue;
		i = gc_block_cells[b];
		while (i--) {
			c = base + i * CELL__Sz;
			if (c[CELL_TYPE] == T_STRING && c[CELL_A]) free((char *)c[CELL_A]);
			c[CELL_TYPE] = T_NIL;
		}
		free(base);
		gc_block[b] = 0;
		gc_block_cells[b] = 0;
	}
	gc_nblocks = 0;
	gc_ncells = 0;
	gc_free_count = 0;
	gc_freelist = 0;
	gc_root_genv = gc_root_a = gc_root_b = gc_root_c = 0;
	gc_root_true = gc_root_false = 0;
	if (gc_marks)    { free(gc_marks);              gc_marks = 0; }
	if (gc_worklist) { free((char *)gc_worklist);   gc_worklist = 0; }
	if (gc_extra)    { free((char *)gc_extra);      gc_extra = 0; }
	gc_nextra = 0;
	// The global-environment index lives in cells.h and outlives the
	// arena it indexes; there is nothing left to look up now.
	if (env_gidx) free(env_gidx);
	env_gidx = 0;
	env_gidx_cap = 0;
	env_gidx_off = 0;
}

// Initialize the arena and thread the free list. Returns 0 on success.
// NCELLS is the first block; the rest arrive as they are needed.
int gc_init (int ncells) {
	gc_nextra = 0;
	if (!(gc_extra = malloc(GC_EXTRA_MAX * sizeof(int)))) {
		printf("c4sp: cannot allocate the root table\n");
		return 1;
	}
	gc_nblocks = 0;
	gc_ncells = 0;
	gc_freelist = 0;
	gc_free_count = 0;
	gc_marks = 0;
	if (!(gc_worklist = malloc(GC_WL_MAX * sizeof(int)))) {
		printf("c4sp: cannot allocate the mark worklist\n");
		return 1;
	}
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

// Whether a cell of type T holds cells: two (A, B), three (A, B, C), or
// none. Typed, not conservative: only the slots that hold cell references
// per the table in cell.h are followed. A cell retained by a stale stack
// word is on the free list with type T_NIL, so it holds none.
int gc_kids (int t) {
	if (t == T_CONS || t == T_ENV || t == T_CONT) return 2;
	// Closures and kont frames: every used slot is a cell or 0
	if (t == T_LAMBDA || t == T_MACRO || t == T_FASTMACRO ||
	    (t >= T_KARG && t <= T_KMACRO)) return 3;
	return 0;
}

// Mark one candidate cell, and queue it if it has children to visit.
void gc_mark (int w) {
	int idx, bit;
	idx = gc_cellidx(w);
	if (idx < 0) return;
	bit = 1 << (idx & 7);
	if (gc_marks[idx >> 3] & bit) return;
	gc_marks[idx >> 3] = gc_marks[idx >> 3] | bit;
	if (!gc_kids(((int *)w)[CELL_TYPE])) return;
	if (gc_wl_top < GC_WL_MAX) gc_worklist[gc_wl_top++] = w;
	else gc_wl_over = 1;
}

// Mark a cell's children. B is queued (and C), A is walked here, and so
// on down: a list's car goes first and its cdr waits, one queued cdr for
// each level the walk goes down, and when that cdr comes off the queue
// it takes its own place there for the cdr after it. So what is queued
// at once is about as many cells as the data nests deep, however long
// its lists (docs/c4sp-design.md 4.8).
void gc_trace (int *c) {
	int k, idx, bit;
	while (1) {
		k = gc_kids(c[CELL_TYPE]);
		if (!k) return;
		gc_mark(c[CELL_B]);
		if (k == 3) gc_mark(c[CELL_C]);
		idx = gc_cellidx(c[CELL_A]);
		if (idx < 0) return;
		bit = 1 << (idx & 7);
		if (gc_marks[idx >> 3] & bit) return;
		gc_marks[idx >> 3] = gc_marks[idx >> 3] | bit;
		c = (int *)c[CELL_A];
	}
}

// Visit queued cells' children, and theirs, until nothing marked is left
// unvisited. When the worklist overflowed, the cells left off it are
// marked, so a walk of the arena finds them: every marked cell's children
// are marked again (a no-op for those done already), and the walk repeats
// until one goes by without an overflow.
void gc_drain () {
	int *base;
	int b, i, k;
	while (1) {
		while (gc_wl_top) gc_trace((int *)gc_worklist[--gc_wl_top]);
		if (!gc_wl_over) return;
		gc_wl_over = 0;
		b = 0;
		while (b < gc_nblocks) {
			base = (int *)gc_block[b];
			i = 0;
			while (i < gc_block_cells[b]) {
				k = gc_block_first[b] + i;
				if (gc_marks[k >> 3] & (1 << (k & 7))) {
					gc_trace(base + i * CELL__Sz);
					while (gc_wl_top) gc_trace((int *)gc_worklist[--gc_wl_top]);
				}
				++i;
			}
			++b;
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
	int i, b, k, freed;

	++gc_collections;
	memset(gc_marks, 0, (gc_ncells + 7) / 8);
	gc_wl_top = 0;
	gc_wl_over = 0;

	// Roots: the interpreter globals...
	gc_mark((int)gc_root_genv);
	gc_mark((int)gc_root_true);
	gc_mark((int)gc_root_false);
	gc_mark((int)gc_root_a);
	gc_mark((int)gc_root_b);
	gc_mark((int)gc_root_c);
	// ...the globals compiled code registered...
	i = 0;
	while (i < gc_nextra) { gc_mark(*(int *)gc_extra[i]); ++i; }
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
			k = gc_block_first[b] + i;
			if (gc_marks[k >> 3] & (1 << (k & 7))) ++gc_last_live;
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
		// Where memory will not give that much -- c4sc inside C4IX's 64 MB
		// machine -- half as much, and so on: more collections, but the run
		// goes on, and stops only when not a cell is free.
		while (i >= 256 && gc_addblock(i)) i = i / 2;
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

// Proof-of-concept: cooperative task switching using ONLY the LEV instruction,
// i.e. without any access to the sp/bp VM registers (the c4lm constraint).
//
// Key idea: LEV does  sp = bp; bp = *sp++; pc = *sp++
// so a frame's bp IS the address of a (saved_bp, return_pc) pair, and LEV
// re-derives sp from bp. One LEV alone leaves sp pointing into the OLD task's
// stack; a second LEV (a "trampoline" function that is nothing but LEV) fixes
// sp because it re-derives it from the NEW bp.
//
// Run: ./c4m coop.c        or      ./c4 c4m.c coop.c

int *cur;          // current task
int *ta, *tb;      // two tasks
int *tramp;        // address of the trampoline's LEV

// Task struct: [0] = frame address (its bp), [1] = saved bp value, [2] = saved pc
enum { T_BP, T_SAVEDBP, T_SAVEDPC, T__Sz };

int trampoline () { }   // compiles to: ENT 0 / LEV   -> tramp+2 is the LEV

int task_done () {
	printf("task_done: a task ran off the end. Exiting.\n");
	exit(0);
}

int switch_to (int *next) {
	int *bp;      // first local -> lives at bp-1, so &bp+1 == bp
	int *n;

	bp = (int *)(&bp + 1);

	// Save the outgoing task: its frame address and the pair living there
	cur[T_BP]      = (int)bp;
	cur[T_SAVEDBP] = *bp;
	cur[T_SAVEDPC] = *(bp + 1);

	// Restore the incoming task's own (saved_bp, return_pc) pair
	n = (int *)next[T_BP];
	*n       = next[T_SAVEDBP];
	*(n + 1) = next[T_SAVEDPC];

	// Point our own frame at the incoming task, returning via the trampoline
	*bp       = next[T_BP];
	*(bp + 1) = (int)(tramp + 2);

	cur = next;
	// LEV here: bp = next's frame, pc = trampoline's LEV, sp still bogus.
	// The trampoline's LEV then sets sp = bp and pops the real pair. Consistent.
	return 0;
}

int task_a (int x, int y) {
	int i;
	printf("A: started with x=%d y=%d\n", x, y);
	i = 0;
	while (i++ < 3) {
		printf("A: iteration %d\n", i);
		switch_to(tb);
	}
	printf("A: finished\n");
	switch_to(tb);
	return 0;
}

int task_b (int x, int y) {
	int i;
	printf("B: started with x=%d y=%d\n", x, y);
	i = 0;
	while (i++ < 4) {
		printf("B: iteration %d\n", i);
		switch_to(ta);
	}
	printf("B: finished\n");
	return 0;
}

// Build a task that has never run: fake the (saved_bp, return_pc) pair so that
// the trampoline's LEV "returns" straight into the entry point's ENT.
int *mk_task (int *entry, int x, int y) {
	int *t, *stk, *X;

	t   = malloc(sizeof(int) * T__Sz);
	stk = malloc(sizeof(int) * 256);
	memset(stk, 0, sizeof(int) * 256);
	X = stk + 200;

	X[0] = 0;                  // saved bp (junk; ENT overwrites X[1] anyway)
	X[1] = (int)entry;         // return pc -> entry point
	X[2] = (int)&task_done;    // where the entry function's own LEV will land
	X[3] = y;                  // after ENT, bp == X+1, so bp+2 == X+3 == 2nd arg
	X[4] = x;                  //                          bp+3 == X+4 == 1st arg

	t[T_BP]      = (int)X;
	t[T_SAVEDBP] = X[0];
	t[T_SAVEDPC] = X[1];
	return t;
}

int main () {
	tramp = (int *)&trampoline;
	cur = malloc(sizeof(int) * T__Sz);
	memset(cur, 0, sizeof(int) * T__Sz);

	ta = mk_task((int *)&task_a, 11, 22);
	tb = mk_task((int *)&task_b, 33, 44);

	printf("main: switching to A\n");
	switch_to(ta);
	printf("main: back in main, switching to B\n");
	switch_to(tb);
	printf("main: unreachable in this test\n");
	return 0;
}

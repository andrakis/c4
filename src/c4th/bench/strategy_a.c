// Strategy (a) modelled faithfully: a software data stack, and one
// subroutine call per Forth word. This is what "JSR &prim_add" costs
// once the primitive has to reach the stack through memory.
int *stk;
int  sp;

void pushv (int v) { stk[sp] = v; sp = sp + 1; }
int  popv  ()      { sp = sp - 1; return stk[sp]; }
void p_add ()      { int a, b; b = popv(); a = popv(); pushv(a + b); }

int main () {
	int i, c0, c1;
	stk = malloc(1024 * sizeof(int));
	sp  = 0;
	c0 = __c4_cycles();
	pushv(0);
	i = 0;
	while (i < 100000) {
		pushv(i);        // the Forth word I
		p_add();         // the Forth word +
		i = i + 1;
	}
	c1 = __c4_cycles();
	printf("strategy(a) cycles: %d  (sum %d)\n", c1 - c0, popv());
	return 0;
}

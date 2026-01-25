#ifndef __C4CC_HELPERS_H
#define __C4CC_HELPERS_H 1

// Helper function to print the stack values
#define STACK_DEPTH 16
#define STACK_WIDTH  8
static void __print_stack (int *sp) {
	int *s, c;
	c = STACK_DEPTH;
	s = sp ? sp : (int *)&sp;
	while (c--) {
		printf("  %16lx", *s++);
		if (c > 0 && (c % STACK_WIDTH == 0))
			printf("\n");
	}
	printf("\n");
}

#undef STACK_DEPTH
#undef STACK_WIDTH

#endif

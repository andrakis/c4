// overrun.c - break this task's own stack guard, on purpose, two ways.
//
//   overrun          recurse until a frame reaches into the guard under
//                    the 256 KB stack, then come back and exit
//   overrun stray    write one word into the guard from main, and exit
//
// What it pins is C4KE's REPORT, which must tell the two apart. Both
// leave SP near the top of the stack when the kernel looks, at exit,
// and the old report said only that ("44 of 262144 bytes, guard
// broken"): it was read as a stray write when the climb's real case was
// c4sc recursing 4,671 frames deep and coming back between two checks.
// The kernel now reads the stack's own high-water mark (it is zeroed at
// start), so the recursion says "used to the bottom" and the stray write
// says "never used below N bytes". src/c4ke/c4ke.c, kernel_stack_check.
//
// The recursion stops a little INSIDE the guard rather than going on
// through it, so the heap block below is not touched and the shell that
// started this survives to run the next command. The guard is 256
// bytes, the stack's top is a little above main's first local, and a
// frame here is 76 bytes, so stopping 100 bytes above where the stack's
// bottom would be if main's local were at the very top lands the last
// frame between the stack's bottom and the guard's.

enum { STACK = 262144 };

int stop;

int deep (int n) {
	int pad[16];
	pad[0] = n; pad[7] = n; pad[15] = n;
	if ((int)pad < stop) return n;
	return deep(n + 1) + pad[0] - pad[15];
}

int main (int argc, char **argv) {
	int top, n, *w;
	top = (int)&top;
	if (argc > 1 && *argv[1] == 's') {
		// 32 bytes above top - STACK is inside the guard: below the
		// stack's bottom (top is under the stack's real top by main's
		// frame and argv), above the guard's base (256 bytes lower).
		w = (int *)(top - STACK + 32);
		*w = 12345;
		printf("overrun: one stray word in the guard\n");
		return 0;
	}
	stop = top - STACK + 100;
	n = deep(0);
	printf("overrun: %d frames deep, and back\n", n);
	return 0;
}

// C4 Test: variable function arguments, manually implemented.
//          This test allowed for implementation of variadic functions in C4CC.
//
// In C4, calling a function with too many arguments offsets the
// stack in such a way that normal parameters are not the correct values.
// Eg:   int add (int a, int b) { return a + b; }
//       add(1, 2);        // Returns 3
//       add(1, 2, 3);     // Return 5 (just the last 2 arguments added together)
//
// Support has been added into C4CC that does what the va_testn functions do manually.
// See tests/vararg2.c for a version that uses stdarg.h

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef C4CC
#define int long
#define __opcode(x) 0
#endif

#define VSTACK_MAX 64
int *_vstack;
int  _vptr;

// Manipulation of the _vstack and _vptr variables
#define vpush(v)    _vstack[_vptr++] = v
#define vpop()      _vstack[--_vptr]
#define vadj(n)     _vptr = _vptr + n
#define vptr()      &_vstack[_vptr]

// The traditional va_arg and related macros. These use the above macros.
#define va_list int *
#define va_arg(AP, TYPE)   (AP = AP + 1, *((TYPE *) (AP - 1)))
// va_start does an implicit va_arg to skip the count.
// va_end undoes this to get back to the count.
#define va_start(AP, LAST) (AP = *(&LAST - 1), va_arg(AP, int))
#define va_end(AP)         (AP = AP - 1, vadj(-(1 + va_arg(AP, int))))

// Helper function to print the stack values
enum { STACK_DEPTH = 16, STACK_WIDTH = 8 };
void print_stack (int *sp) {
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

// Implementation of add_numbers that takes a va_list.
// See add_numbers for the variadic function version.
int vadd_numbers (int count, va_list args) {
	int acc, v;
	acc = 0;
	printf("Count is: %ld, args is at 0x%lx\n", count, args);
	while (count--) {
#if 1
		v = va_arg(args, int);
		printf("got v: %ld, acc: %ld\n", v, acc);
		acc = acc + v;
#else
		acc = acc + va_arg(args, int);
#endif
	}
	return acc;
}

// Implementation that calls vadd_numbers using the va_start
// and va_end macros.
int add_numbers (int count, ...) {
	va_list args;
	int i;

	printf("add_numbers: got count %ld\n", count);
	if (count > 50) { printf("Aborting\n"); return 0; }
	print_stack(&count - 1);

	va_start(args, count);
	printf("add_numbers, va_start args is at 0x%lx\n", args);
	i = vadd_numbers(count, args);
	printf("add_numbers, cleaning va_args pointer at 0x%lx\n", args);
	va_end(args);

	return i;
}

int vterrible_printf (char *fmt, va_list args) {
	char *str;
	int   flags;
	str = fmt;
	flags = 0;
	while (*fmt) {
		if (*fmt != '%') {
			++str;
		} else {
			flags = 0;
		}
	}
}

// Helper macro that casts code to the appropriate function signature, which
// C4CC does not understand.
#ifndef C4CC
#define code()   (int)(((int (*)())code))
#endif
int  mva_count, *mva_ptr, *mva_arg, mva_n, mva_v;
// A call to this function is inserted into code that calls variadic functions,
// along with the parameter count, and some ADJ'ment as needed.
// The above globals are required as our bp is at a random offset, depending
// on number of arguments pushed.
// Globals on the other hand, are hard-coded addresses.
int *make_va (int count) {
	mva_count = mva_n = count;
	mva_arg   = &count + count;
	mva_ptr   = vptr();

	printf("(make_va with count: %ld)\n", mva_count);
	print_stack(0);

	// Push the count as the first argument. This is skipped by va_start.
	printf("(make_va: pushing count %ld to arg list)\n", mva_count);
	vpush(mva_count);

	while (mva_n--) {
		mva_v = *mva_arg;
		printf("(make_va: pushing value %ld to arg list)\n", mva_v);
		vpush(mva_v);
		--mva_arg;
	}

	printf("(make_va: returning arg pointer 0x%lx (%ld)\n", mva_ptr, mva_ptr);

	return mva_ptr;
}

// Call a vararg function that passes its arguments to vadd_numbers
int va_test1 () {
	int *code, *cp, len, r, *vp, count;
	int ENT, IMM, PSH, JSR, ADJ, LEV;
	len = sizeof(int) * 64; // overkill
	if (!(code = cp = malloc(len))) {
		printf("va_test malloc failure for %ld bytes\n", len);
		return 0;
	}
	--cp;
	ENT = __opcode("ENT"); IMM = __opcode("IMM"); PSH = __opcode("PSH");
	JSR = __opcode("JSR"); ADJ = __opcode("ADJ"); LEV = __opcode("LEV");
	count = 2;
	*++cp = ENT; *++cp = 0; // ENT 0
	// add_numbers(count, ...)
	*++cp = IMM; *++cp = count; // IMM count
	*++cp = PSH; // PSH
	*++cp = IMM; *++cp = 10; // IMM 10
	*++cp = PSH; // PSH
	*++cp = IMM; *++cp = 20; // IMM 20
	*++cp = PSH; // PSH
	// Here is where we insert our call to make_va
	*++cp = IMM; *++cp = count; // IMM count
	*++cp = PSH; // PSH
	*++cp = JSR; *++cp = (int) &make_va;
	*++cp = ADJ; *++cp = count + 1; // pop args to make_va
	// Push result of call to make_va
	*++cp = PSH;
	// Code proceeds as normal, except that the ADJ only needs to be 2
	*++cp = JSR; *++cp = (int) &add_numbers; // JSR add_numbers
	*++cp = ADJ; *++cp = 2; // ADJ 2
	*++cp = LEV; // LEV
	printf("Invoking code...\n");
	r = code();
	printf("invoked with result: %ld\n", r);
	free(code);
	return r;
}

// Call a vararg function that passes its arguments to vadd_numbers
int va_test2 () {
	int *code, *cp, len, r, *vp, count;
	int ENT, IMM, PSH, JSR, ADJ, LEV;
	len = sizeof(int) * 64; // overkill
	if (!(code = cp = malloc(len))) {
		printf("va_test malloc failure for %ld bytes\n", len);
		return 0;
	}
	--cp;
	ENT = __opcode("ENT"); IMM = __opcode("IMM"); PSH = __opcode("PSH");
	JSR = __opcode("JSR"); ADJ = __opcode("ADJ"); LEV = __opcode("LEV");
	count = 5;
	*++cp = ENT; *++cp = 0; // ENT 0
	// add_numbers(count, ...)
	*++cp = IMM; *++cp = count; // IMM count
	*++cp = PSH; // PSH
	*++cp = IMM; *++cp = 10; // IMM 10
	*++cp = PSH; // PSH
	*++cp = IMM; *++cp = 20; // IMM 20
	*++cp = PSH; // PSH
	*++cp = IMM; *++cp = 30; // IMM 20
	*++cp = PSH; // PSH
	*++cp = IMM; *++cp = 40; // IMM 20
	*++cp = PSH; // PSH
	*++cp = IMM; *++cp = 50; // IMM 20
	*++cp = PSH; // PSH
	// Here is where we insert our call to make_va
	*++cp = IMM; *++cp = count; // IMM count
	*++cp = PSH; // PSH
	*++cp = JSR; *++cp = (int) &make_va;
	*++cp = ADJ; *++cp = count + 1; // pop args to make_va
	// Push result of call to make_va
	*++cp = PSH;
	// Code proceeds as normal, except that the ADJ only needs to be 2
	*++cp = JSR; *++cp = (int) &add_numbers; // JSR add_numbers
	*++cp = ADJ; *++cp = 2; // ADJ 2
	*++cp = LEV; // LEV
	printf("Invoking code...\n");
	r = code();
	printf("invoked with result: %ld\n", r);
	free(code);
	return r;
}

int main () {
	int i;
	va_list args;

	if (!(_vstack = malloc(i = sizeof(int) * VSTACK_MAX))) {
		printf("Alloc failure 1\n");
		return 1;
	}
	memset(_vstack, 0, i);
	_vptr = 0;

	va_test1();
	va_test2();

	printf("_vptr(%ld) == %s\n", _vptr, _vptr ? "failure" : "success!");

	free(_vstack);

	return 0;
}

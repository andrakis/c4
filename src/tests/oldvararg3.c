// C4 Test: variable function arguments
//
// In C4, calling a function with too many arguments offsets the
// stack in such a way that normal parameters are not the correct values.
// Eg:   int add (int a, int b) { return a + b; }
//       add(1, 2);        // Returns 3
//       add(1, 2, 3);     // Return 5 (just the last 2 arguments added together)
//
// what if..
//  we create a stub:
//  malloc some code array, and fill it with
//    PSH 3     ; count (for subroutine)
//    PSH 1     ; va arg
//    PSH 2     ; ..
//    PSH 3     ; ..
//    PSH 3     ; number of items on stack to turn into va args
//    custom_op MAKE_VA 3
//    JSR func
// MAKE_VA takes *sp values, moves them into vstack,
// adjusts SP to remove given number of values,
// and pushes vptr() onto stack.
// - then call code array

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef C4CC
#define int long
#endif

#ifndef __U0_C

// C4INFO state
enum {
	C4I_NONE = 0x0,  // No C4 info
	C4I_C4   = 0x1,  // Ultimately running under C4
	C4I_C4M  = 0x2,  // Running under c4m (directly or C4)
	C4I_C4P  = 0x4,  // Running under c4plus
	C4I_C4MJS= 0x8,  // Running under C4M.JS host
	C4I_HRT  = 0x10, // High resolution timer
	C4I_SIG  = 0x20, // Signals supported
	C4I_FLT  = 0x40, // Floating point instruction support
	C4I_PROT = 0x80, // Protected mode support
	C4I_C4KE = 0x200, // C4KE is running
};

enum { MAKE_VA = 180, VADJ = 181 };

#ifndef C4CC
#define __c4_opcode(...)
#define __opcode(s)  0
#define __c4_info() 0
int install_trap_handler(int *u) { return 0; }
#endif

#endif

int add_numbers (int count, ...) {
	int acc; // accumulator
	int *arg;

	// Fix bp
	// adj_bp(1 - arg_count);
	printf("Arg count: %ld\n", count);

	acc = 0;
	arg = &count + 1;
	while (--count) {
		acc = acc + *arg;
		arg = arg + 1;
		printf("..\n");
	}

	//adj_bp(arg_count - 1);
	return acc;
}

#define va_list int *
#define va_arg(AP, TYPE)   (AP = AP + 1, *((TYPE *) (AP - 1)))
int vadd_numbers (int count, va_list args, ...) {
	int acc, v;
	acc = 0;
	printf("Count is: %ld, args is at 0x%X\n", count, args);
	while (count--) {
#if 1
		v = va_arg(args, int);
		printf("got v: %d, acc: %d\n", v, acc);
		acc = acc + v;
#else
		acc = acc + va_arg(args, int);
#endif
	}
	return acc;
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

#define VSTACK_MAX 64
int *_vstack;
int  _vptr;
#define NO_INLINE 0
#if NO_INLINE
void  vpush (int v) { _vstack[_vptr++] = v; }
int   vpop  ()      { return _vstack[--_vptr]; }
void  vadj  (int n) { _vptr = _vptr + n; }
int  *vptr  ()      { return &_vstack[_vptr]; }
#else
#define vpush(v)    _vstack[_vptr++] = v
#define vpop()      _vstack[--_vptr]
#define vadj(n)     _vptr = _vptr + n
#define vptr()      &_vstack[_vptr]
#endif

enum { STACK_DEPTH = 8, STACK_WIDTH = 8 };
void print_stack (int *sp) {
	int *s, c;
	c = STACK_DEPTH;
	s = sp ? sp : (int *)&sp;
	while (c--) {
		printf("  %08X", *s++);
		if (c > 0 && (c % STACK_WIDTH == 0))
			printf("\n");
	}
	printf("\n");
}

void custom_op (int trap, int ins, int mode, int a, int *bp, int *sp, int *returnpc) {
	int n, *nsp, *vp;

	printf("  Trap handler: T%d  I%d(0x%X) mode%d\n", trap, ins, ins, mode);
	printf("  SP=0x%X  BP=0x%X  ReturnPC=0x%X\n", sp, bp, returnpc);

	print_stack(sp);

	// MAKE_VA N - Move N stack values to a va_list, then push pointer to them
	//             onto the stack.
	// When pushing values to va_list, starts at sp+Count-1, ie the
	// first item pushed, and works it way downwards to the bottom of the stack.
	if (ins == MAKE_VA) {
		n = *returnpc;      // get count
		vp = vptr();        // get a pointer to a va_list
		// Start at deepest stack value and work downwards
		nsp = sp + n - 1;
		printf("  MAKE_VA(%ld), first arg at 0x%X = %ld, vp at 0x%X\n", n, nsp, *nsp, vp);
		while (n--) {
			printf("pushing value %ld to arg list\n", *nsp);
			vpush(*nsp--);
		}

		// now adjust nsp to be at deepest level again
		nsp = sp + *returnpc++ - 1;
		// now an implicit PSH of the vptr
		*nsp = (int) vp;
		sp = nsp;

		printf("  final sp = 0x%X\n", sp);
		print_stack(sp);
	// VADJ N - Adjust the va_list stack by N to remove pushed arguments
	} else if (ins == VADJ) {
		printf("  vadj(%d)\n", *returnpc);
		vadj(*returnpc++);
	} else {
		printf("Something went wrong, bailing!\n");
		exit(-1);
	}
}


#ifndef C4CC
#define code()   (int)(((int (*)())code))
#endif
int test_add2 (int a, int b) { return a + b; }
int va_test1 () {
	int *code, *cp, len, r;
	int ENT, IMM, PSH, JSR, ADJ, LEV;
	len = sizeof(int) * 64; // overkill
	if (!(code = cp = malloc(len))) {
		printf("va_test malloc failure for %ld bytes\n", len);
		return 0;
	}
	--cp;
	ENT = __opcode("ENT"); IMM = __opcode("IMM"); PSH = __opcode("PSH");
	JSR = __opcode("JSR"); ADJ = __opcode("ADJ"); LEV = __opcode("LEV");
	*++cp = ENT; *++cp = 0; // ENT 0
	*++cp = IMM; *++cp = 1; // IMM 1
	*++cp = PSH; // PSH
	*++cp = IMM; *++cp = 2; // IMM 2
	*++cp = PSH; // PSH
	*++cp = JSR; *++cp = (int) &test_add2; // JSR test_add2
	*++cp = ADJ; *++cp = 2; // ADJ 2
	*++cp = LEV; // LEV
	printf("Invoking code...\n");
	r = code();
	printf("invoked with result: %ld\n", r);
	free(code);
	return r;
}

// Now do it again, but make them va_args
int va_test2 () {
	int *code, *cp, len, r, *vp, count;
	int ENT, IMM, PSH, JSR, ADJ, LEV, OR;
	len = sizeof(int) * 64; // overkill
	if (!(code = cp = malloc(len))) {
		printf("va_test malloc failure for %ld bytes\n", len);
		return 0;
	}
	--cp;
	ENT = __opcode("ENT"); IMM = __opcode("IMM"); PSH = __opcode("PSH");
	JSR = __opcode("JSR"); ADJ = __opcode("ADJ"); LEV = __opcode("LEV");
	OR  = __opcode("OR");
	count = 2;
	*++cp = ENT; *++cp = 0; // ENT 0
	// vadd_numbers(2, ...)
	*++cp = IMM; *++cp = count; // IMM count
	*++cp = PSH; // PSH
	*++cp = IMM; *++cp = 1; // IMM 1
	*++cp = PSH; // PSH
	*++cp = IMM; *++cp = 2; // IMM 2
	*++cp = PSH; // PSH
	*++cp = MAKE_VA; *++cp = count; // MAKE_VA count
	*++cp = JSR; *++cp = (int) &vadd_numbers; // JSR vadd_numbers
	*++cp = ADJ; *++cp = 2; // ADJ 2
	*++cp = VADJ; *++cp = -count; // VADJ -count
	*++cp = LEV; // LEV
	printf("Invoking code...\n");
	r = code();
	printf("invoked with result: %ld\n", r);
	free(code);
	return r;
}

// Third test, larger numbers
int va_test3 () {
	int *code, *cp, len, r, *vp, count;
	int ENT, IMM, PSH, JSR, ADJ, LEV, OR;
	len = sizeof(int) * 64; // overkill
	if (!(code = cp = malloc(len))) {
		printf("va_test malloc failure for %ld bytes\n", len);
		return 0;
	}
	--cp;
	ENT = __opcode("ENT"); IMM = __opcode("IMM"); PSH = __opcode("PSH");
	JSR = __opcode("JSR"); ADJ = __opcode("ADJ"); LEV = __opcode("LEV");
	OR  = __opcode("OR");
	count = 5;
	*++cp = ENT; *++cp = 0; // ENT 0
	// vadd_numbers(5, 1, 2, 3, 4, 5)
	*++cp = IMM; *++cp = count; // IMM count
	*++cp = PSH; // PSH
	*++cp = IMM; *++cp = 1; // IMM 1
	*++cp = PSH; // PSH
	*++cp = IMM; *++cp = 2; // IMM 2
	*++cp = PSH; // PSH
	*++cp = IMM; *++cp = 3; // IMM 3
	*++cp = PSH; // PSH
	*++cp = IMM; *++cp = 4; // IMM 4
	*++cp = PSH; // PSH
	*++cp = IMM; *++cp = 5; // IMM 5
	*++cp = PSH; // PSH
	*++cp = MAKE_VA; *++cp = count; // MAKEVA count
	*++cp = JSR; *++cp = (int) &vadd_numbers; // JSR vadd_numbers
	*++cp = ADJ; *++cp = 2; // ADJ 2
	*++cp = VADJ; *++cp = -count; // VADJ -count
	*++cp = LEV; // LEV
	printf("Invoking code...\n");
	r = code();
	printf("invoked with result: %ld\n", r);
	free(code);
	return r;
}

int main () {
	int old_handler, i;
	va_list args;

	old_handler = 0;
	printf("info: 0x%d\n", __c4_info());
	if (!(__c4_info() & C4I_C4KE)) {
		printf("Installing our custom instruction...\n");
		old_handler = install_trap_handler((int *)&custom_op);
	}
	if (!(_vstack = malloc(i = sizeof(int) * VSTACK_MAX))) {
		printf("Alloc failure 1\n");
		return 1;
	}
	memset(_vstack, 0, i);

	//arg_count = 2; some_test(1, 2);
	//arg_count = 3; some_test(1, 2, 3);
	//arg_count = 4; some_test(1, 2, 3, 4);

	//arg_count = 3; printf("add_numbers: %d\n", add_numbers(2, 1, 2));
	//arg_count = 4; printf("add_numbers: %d\n", add_numbers(3, 1, 2, 3));

	//args = vptr();
	//vpush(1); vpush(2); vpush(3);
	//printf("vadd_numbers(%d): %ld\n", 3, vadd_numbers(3, args));
	//vadj(-3);
	//va_test1();
	//va_test2();
	//va_test3();

	// This will crash C4CC until proper support is added
	printf("calling with var args: %d\n", vadd_numbers(5, 1, 2, 3, 4, 5));

	free(_vstack);

	return 0;
}

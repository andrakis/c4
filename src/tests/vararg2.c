// C4 Test: variable function arguments, using C4CC.
//
// Support has been added into C4CC such that variadic functions now work.
//

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#define VERBOSE 0

#if VERBOSE
#include <c4cc_helpers.h> // __print_stack()
#endif

// Implementation of add_numbers that takes a va_list.
// See add_numbers for the variadic function version.
int vadd_numbers (int count, va_list args) {
	int acc, v;
	acc = 0;
#if VERBOSE
	printf("Count is: %ld, args is at 0x%lx\n", count, args);
#endif

	while (count--) {
		acc = acc + va_arg(args, int);
	}
	return acc;
}

// Implementation that calls vadd_numbers using the va_start
// and va_end macros.
int add_numbers (int count, ...) {
	va_list args;
	int i;

	va_start(args, count);
	i = vadd_numbers(count, args);
	va_end(args);

	return i;
}

int main () {
	int i;

	// Now working in C4CC:
	i = add_numbers(5, 10, 20, 30, 40, 50);
	printf("calling with var args: %d\n", i);
	printf("Another test: %d\n", add_numbers(3, 10, 20, 30));
	return 0;
}


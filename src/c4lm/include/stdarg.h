//
// C4 Standard Library: stdarg.h
//
// Adds the usual va_list type, va_arg, va_start, va_end, and va_copy.
//

#ifndef __STDARG_H
#define __STDARG_H 1

#include <stddef.h> // size_t
#include <stdlib.h> // malloc, free
#include <string.h> // memset

#define __C4CC_VA_STACK 64
static int *__c4cc_va_stack;
static int  __c4cc_va_vptr;

// Adjustment of the __c4cc_va_stack and __c4cc_va_vptr variables.
// Note: the first macro is a multi-statement macro, but not enclosed in a
// do/while(0) loop, as C4CC doesn't understand this construct.
#define __c4cc_va_vpush(v)    __c4cc_va_stack[__c4cc_va_vptr] = (v); ++__c4cc_va_vptr
#define __c4cc_va_vadj(n)     __c4cc_va_vptr = __c4cc_va_vptr - (n)
#define __c4cc_va_vptr()      &__c4cc_va_stack[__c4cc_va_vptr]

// The traditional va_arg and related macros. These use the above adjustment macros.
#define va_list int *
#define va_arg(AP, TYPE)   (AP = AP + 1, *((TYPE *) (AP - 1)))
// va_start does an implicit va_arg to skip the count.
// va_end undoes this to get back to the count.
#define va_start(AP, LAST) (AP = (va_list) *(&LAST - 1), va_arg(AP, int))
#define va_end(AP)         (AP = AP - 1, __c4cc_va_vadj(1 + va_arg(AP, int)))
#define va_copy(D, S)      (D = (S))

// These variables are used by __c4cc_make_va, as local variables cannot be used.
static int *__c4cc_va_m_ptr, *__c4cc_va_m_arg, __c4cc_va_m_n, __c4cc_va_m_v;

// A call to this function is inserted by C4CC into code that calls variadic functions,
// along with the parameter count.
// The above globals are required as our bp is at an unknown offset, depending
// on number of arguments pushed. Using local variables would overwrite values
// on the stack.
// Globals on the other hand, are hard-coded addresses instead of references to bp.
// The count parameter still works, as it references the last item on the stack.
// Similarly, variadic functions can still use their arguments as normal.
static int *__c4cc_make_va (size_t count) {
	__c4cc_va_m_n   = count;
	__c4cc_va_m_arg = &count + count;
	__c4cc_va_m_ptr = __c4cc_va_vptr();

	// Push the count as the last argument. This is used by va_end.
	__c4cc_va_vpush(count);

	// Push all arguments in reverse order
	while (__c4cc_va_m_n--) {
		__c4cc_va_vpush(*__c4cc_va_m_arg);
		--__c4cc_va_m_arg;
	}

	// Return the start of the list
	return __c4cc_va_m_ptr;
}

// Constructor added to allocate the var args stack.
static void __attribute__((constructor)) __c4cc_va_constructor () {
	int i;
	if (!(__c4cc_va_stack = malloc(i = sizeof(int) * __C4CC_VA_STACK))) {
		printf("stdarg.h: out of memory attempting to allocate %d bytes\n", i);
		exit(-100);
	}
	memset(__c4cc_va_stack, 0, i);
	__c4cc_va_vptr = 0;
}

// Destructor to free the var args stack.
static void __attribute__((destructor)) __c4cc_va_destructor () {
	if (__c4cc_va_stack) free(__c4cc_va_stack);
}

#endif // ifndef __STDARG_H

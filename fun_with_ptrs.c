// Fun with pointers
// Aimed at C4, but designed to work on gcc too

#include <memory.h>
#include <stdio.h>
#include <stdlib.h>

#include "c4.h"
// class testclass
enum { member_sum, fun_add, fun_sub, fun_mul, fun_set, fun_factorial, fun_print, _testclass_sz };

// Demonstrate a class system
void object_fun_destructor (int *class) {
		free(class);
}

// Implement a garbage collector
int *gc_stack, *gc_ptr, gc_stacksize, *gc_top;

int *gc_push (int *ptr) {
		*--gc_ptr = (int)ptr;
		return gc_ptr;
}

int *gc_collect (int stack_adj) {
		printf("\ngc_collect(%d)\n", stack_adj);
		while(stack_adj > 0) {
				printf("- gc_ptr = %X (%X)\n", gc_ptr, *gc_ptr);
				if(*gc_ptr)
						object_fun_destructor(*gc_ptr++);
				--stack_adj;
		}
		return gc_ptr;
}

int *testclass_fun_add (int *self, int b) {
		self[member_sum] = self[member_sum] + b;
		return self;
}

int *testclass_fun_sub (int *self, int b) {
		self[member_sum] = self[member_sum] - b;
		return self;
}

int *testclass_fun_mul (int *self, int b) {
		self[member_sum] = self[member_sum] * b;
		return self;
}

int *testclass_fun_set (int *self, int b) {
		self[member_sum] = b;
		return self;
}

// forward reference
int *new_testclass;

#define new_testclass(x) ((int*(*)(int))new_testclass)(x)
int testclass_factorial (int *self) {
		int *stack, *n, *f, r;

		stack = gc_top;

		// Create new testclass with number
		if (!(n = new_testclass(self[member_sum])))
				exit(2);

		// if (n <= 0) return 1
		// return n * factorial(n - 1)
}

int *testclass_fun_print (int *self) {
		printf("testclass{sum=%d}", self[member_sum]);
		return self;
}

int *testclass_fun_constructor (int *self, int initial) {
		self[member_sum] = initial;
		self[fun_add] = (int)&testclass_fun_add;
		self[fun_sub] = (int)&testclass_fun_sub;
		self[fun_print] = (int)&testclass_fun_print;
		return self;
}

int testclass_fun_destructor (int *self) {
		// Just run normal destruct
		object_fun_destructor(self);
		return 0;
}

int *impl_new_testclass (int initial) {
		int *c;

		if (!(c = malloc(sizeof(int) * _testclass_sz)))
				return c;
		return testclass_fun_constructor(c, initial);
}

int main (int argc, char **argv) {
		int i, *t1, *t2, *s1, *f;

		gc_stacksize = 1024 * 32;
		if (!(gc_stack = malloc(i = gc_stacksize * sizeof(int)))) {
				printf("Unable to allocate %d bytes for stack\n", i);
				return 1;
		}
		memset(gc_stack, 0, i);
		gc_ptr = gc_stack + gc_stacksize - 1;
		gc_top = gc_ptr;

		new_testclass = (int*)&impl_new_testclass;
		s1 = gc_push(t1 = new_testclass(1));
		s1 = gc_push(t2 = new_testclass(5));

#undef f
#define f(x) ((int (*)(int*))f)(x)
		f = (int*)t1[fun_print]; f(t1);
		f = (int*)t2[fun_print]; f(t2);

		// Calculate factorial
#undef f
#define f(x,y) ((int (*)(int*,int))f)(x,y)
		f = (int*)t1[fun_set]; f(t1, 10);
		                       f(t2, 10);
#undef f
#define f(x) ((int (*)(int*))f)(x)
		f = (int*)t1[fun_factorial]; f(t1);
		f = (int*)t2[fun_factorial]; f(t2);

#undef f
#define f(x) ((int (*)(int*))f)(x)
		f = (int*)t1[fun_print]; f(t1);
		f = (int*)t2[fun_print]; f(t2);

		gc_collect(gc_top - s1);
		free(gc_stack);
		return 0;
}

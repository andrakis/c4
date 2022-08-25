// Classes for C4
// Really ugly
//
// Implements a class framework as well as garbage collection.
//
// Internally, classes are represented as:
//  (malloc'd class structure)
//    word: Magic number (0xBEAF)
//    word: Pointer to destructor
//    word: (malloc'd class data + 1 word)
//      word: ptr to class structure
//      word,...: class member data
//  When an object is created, the member data pointer is returned.
//  When an object is destructed, one can pass either the class structure or the class member
//   data. Since the ptr to class structure is contained 1 word prior to this pointer, we can grab
//   that and use it.
//   The magic number must match for destruction to occur.
//  C4 can only call function pointers that are stored in int* variables. For instance, code
//   like this will not work: ptr[Obj_Destructor](ptr).
//   You first need to assign ptr[Obj_Destructor] to a variable, or call a function like invoke1
//   which has access to the pointer as a parameter and doesn't need a temporary store.
//
// Non-C4 notes:
//  Ugly macro hackery is used to avoid constructs C4 doesn't understand. See the ptr macros
//  in the invokeX functions. The hackery here at least is confined to one section.

#include <memory.h>
#include <stdio.h>
#include <stdlib.h>

#include "c4.h"

// VC++: ignore unknown pragmas
#pragma warning(disable: 4068)
// Disable formatting warnings
#pragma warning(disable: 4477)
#pragma warning(disable: 6328)
#pragma GCC diagnostic ignored "-Wformat"

enum { Obj_Magic, Obj_Des, Obj_Data, Obj__Sz };
enum { OBJ_MAGIC = 0xBEAF };

//
// Function invocation in a way that C4 and normal C compilers understand.
//
#undef ptr
#define ptr(a) ((int(*)(int))ptr)(a)
int invoke1 (int *ptr, int arg) { return ptr(arg); }
#undef ptr
#define ptr(a,b) ((int(*)(int,int))ptr)(a,b)
int invoke2 (int *ptr, int a, int b) { return ptr(a, b); }
#undef ptr
#define ptr(a,b,c) ((int(*)(int,int,int))ptr)(a,b,c)
int invoke3 (int *ptr, int a, int b, int c) { return ptr(a, b, c); }
#undef ptr

//
// Object management
//

// Generic object constructor. Internal use.
int *object_construct_ (int size, int *des) {
	int *ptr, *dat;
	if (!(ptr = malloc(Obj__Sz * sizeof(int)))) { printf("object_construct: failed to allocate %d bytes\n", size); exit(-1); }
	ptr[Obj_Magic] = OBJ_MAGIC;
	//ptr[Obj_Con] = (int)cons;
	ptr[Obj_Des] = (int)des;
	if (!(dat = malloc(size + sizeof(int*)))) {
		free(ptr);
		printf("object_construct(data): failed to allocate %d bytes\n", size);
		exit(-2);
	}
	*dat = (int)ptr;
	ptr[Obj_Data] = (int)(dat + 1);
	memset((void*)ptr[Obj_Data], 0, size);
	return ptr;
}

// Call to construct an object with 'size' elements in the data structure,
// and takes no arguments for its constructor.
int *object_construct0 (int size, int *cons, int *des) {
	int *ptr;
	ptr = object_construct_(size, des);
	invoke1(cons, (int)ptr[Obj_Data]);
	return (int*)ptr[Obj_Data];
}

// Call to construct an object with 'size' elements in the data structure,
// and takes 1 arguments for its constructor.
int *object_construct1 (int size, int *cons, int *des, int a) {
	int *ptr;
	ptr = object_construct_(size, des);
	invoke2(cons, (int)ptr[Obj_Data], a);
	return (int*)ptr[Obj_Data];
}

// Call to construct an object with 'size' elements in the data structure,
// and takes 2 arguments for its constructor.
int *object_construct2 (int size, int *cons, int *des, int a, int b) {
	int *ptr;
	ptr = object_construct_(size, des);
	invoke3(cons, (int)ptr[Obj_Data], a, b);
	return (int*)ptr[Obj_Data];
}

// Call to deconstruct the given object. Obtains pointer to object structure
// automatically.
void object_destruct (int *obj) {
	int *ptr;

	ptr = obj;
	if (ptr[Obj_Magic] != OBJ_MAGIC) {
		// Maybe we were passed Obj_Data instead?
		// Grab obj ptr from Obj_Data - 1
		ptr--;
		ptr = (int*)*ptr;
		if (ptr[Obj_Magic] != OBJ_MAGIC) {
			printf("Error: no MAGIC found\n");
			exit(-3);
		}
	}
	if ((int*)ptr[Obj_Des] != (int*)&object_destruct)
		invoke1((int*)ptr[Obj_Des], (int)ptr[Obj_Data]);
	free((int*)ptr[Obj_Data] - 1);
	free(ptr);
}

//
// Garbage collector: stack based
//
int *gc_stack, *gc_ptr, gc_stacksize, *gc_top;

// No checking is done to ensure you don't push multiple of the same class reference.
// If you do, you'll get a double deconstruct attempt and crash.
int *gc_push (int *ptr) {
	//printf("gc_push(%X)\n", ptr);
	*--gc_ptr = (int)ptr;
	return gc_ptr;
}

// Collect given number of objects on the stack (positive integer)
int *gc_collect (int stack_adj) {
	if (stack_adj < 1)
		return gc_ptr;
	printf("gc_collect(%d)\n", stack_adj);
	while(stack_adj > 0) {
		//printf("- gc_ptr = %X (%X)\n", (int)gc_ptr, *gc_ptr);
		if(*gc_ptr)
			object_destruct((int*)*gc_ptr);
		*gc_ptr = 0;
		++gc_ptr;
		--stack_adj;
	}
	return gc_ptr;
}

// Collect all memory up to gc_top
int *gc_collect_top () {
	return gc_collect(gc_top - gc_ptr);
}

// This MUST be called prior to using garbage collections functions.
// Manual memory management does not require this.
int gc_init() {
	int i;

	gc_stacksize = 1024 * 32;
	if (!(gc_stack = malloc(i = gc_stacksize * sizeof(int)))) {
		printf("Unable to allocate %d bytes for stack\n", i);
		return 1;
	}
	memset(gc_stack, 0, i);
	gc_ptr = gc_stack + gc_stacksize - 1;
	gc_top = gc_ptr;

	return 0;
}

// Called once at end of program to free all gc related memory.
// Includes a call to collect anything not yet freed.
void gc_cleanup () {
	gc_collect_top();
	free(gc_stack);
}

//
// Utility functions
//

// Swap two integer pointers
void swap2(int *x, int *y) {
	int tmp;
	// XOR method (slow on c4)
	//*x = *x ^ *y; *y = *y ^ *x; *x = *x ^ *y;
	// Temporary method
	tmp = *x; *x = *y; *y = tmp;
}

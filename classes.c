// Classes for C4
// Really ugly
//
// Implements a class framework as well as (optionally automatic) garbage collection.
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
// Automatic garbage collection is acheived by calling gc_enable_autocollect(). Any new objects are
// automatically pushed onto the garbage collector stack and collected in either of the following ways:
//
//   int *ptr; // Keep track of created objects during this function
//   ptr = gc_enable_autocollect();  // OR
//   ptr = gc_ptr;
//   // calls that make objects
//   gc_cleanup();  // Clean all objects; OR
//   gc_collect(gc_top - ptr); // Clean just the recently created objects
//
// Non-C4 notes:
//  Ugly macro hackery is used to avoid constructs C4 doesn't understand. See the ptr macros
//  in the invokeX functions. The hackery here at least is confined to one section.

#ifndef _C4STDLIB
#define _C4STDLIB 1

#include <memory.h>
#include <stdio.h>
#include <stdlib.h>

#include "c4.h"

// Dummy out stacktrace if not available
void stacktrace () { }
#define stacktrace()

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
#define ptr() ((int(*)())ptr)()
int invoke0 (int *ptr) { return ptr(); }
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
#define ptr(a,b,c,d) ((int(*)(int,int,int,int))ptr)(a,b,c,d)
int invoke4 (int *ptr, int a, int b, int c, int d) { return ptr(a, b, c, d); }
#undef ptr
#define ptr(a,b,c,d,e) ((int(*)(int,int,int,int,int))ptr)(a,b,c,d,e)
int invoke5 (int *ptr, int a, int b, int c, int d, int e) { return ptr(a, b, c, d, e); }
#undef ptr
#define ptr(a,b,c,d,e,f) ((int(*)(int,int,int,int,int,int))ptr)(a,b,c,d,e,f)
int invoke6 (int *ptr, int a, int b, int c, int d, int e, int f) { return ptr(a, b, c, d, e, f); }
#undef ptr
#define ptr(a,b,c,d,e,f,g) ((int(*)(int,int,int,int,int,int,int))ptr)(a,b,c,d,e,f,g)
int invoke7 (int *ptr, int a, int b, int c, int d, int e, int f, int g) { return ptr(a, b, c, d, e, f, g); }
#undef ptr
#define ptr(a,b,c,d,e,f,g,h) ((int(*)(int,int,int,int,int,int,int,int))ptr)(a,b,c,d,e,f,g,h)
int invoke8 (int *ptr, int a, int b, int c, int d, int e, int f, int g, int h) { return ptr(a, b, c, d, e, f, g, h); }
#undef ptr
#define ptr(a,b,c,d,e,f,g,h,i) ((int(*)(int,int,int,int,int,int,int,int,int))ptr)(a,b,c,d,e,f,g,h,i)
int invoke9 (int *ptr, int a, int b, int c, int d, int e, int f, int g, int h, int i) { return ptr(a, b, c, d, e, f, g, h, i); }
#undef ptr
#define ptr(a,b,c,d,e,f,g,h,i,j) ((int(*)(int,int,int,int,int,int,int,int,int,int))ptr)(a,b,c,d,e,f,g,h,i,j)
int invoke10 (int *ptr, int a, int b, int c, int d, int e, int f, int g, int h, int i, int j) { return ptr(a, b, c, d, e, f, g, h, i, j); }
#undef ptr

//
// Object management
//

// Callback when an object is constructed, for automatically adding to garbage collector
int *object_construct_callback;

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
	int *ptr; ptr = object_construct_(size, des);
	invoke1(cons, (int)ptr[Obj_Data]);
	if (object_construct_callback) invoke1(object_construct_callback, ptr[Obj_Data]);
	return (int*)ptr[Obj_Data];
}

// Call to construct an object with 'size' elements in the data structure,
// and takes 1 arguments for its constructor.
int *object_construct1 (int size, int *cons, int *des, int a) {
	int *ptr; ptr = object_construct_(size, des);
	invoke2(cons, (int)ptr[Obj_Data], a);
	if (object_construct_callback) invoke1(object_construct_callback, ptr[Obj_Data]);
	return (int*)ptr[Obj_Data];
}

// Call to construct an object with 'size' elements in the data structure,
// and takes 2 arguments for its constructor.
int *object_construct2 (int size, int *cons, int *des, int a, int b) {
	int *ptr; ptr = object_construct_(size, des);
	invoke3(cons, (int)ptr[Obj_Data], a, b);
	if (object_construct_callback) invoke1(object_construct_callback, ptr[Obj_Data]);
	return (int*)ptr[Obj_Data];
}

// Call to construct an object with 'size' elements in the data structure,
// and takes 3 arguments for its constructor.
int *object_construct3 (int size, int *cons, int *des, int a, int b, int c) {
	int *ptr; ptr = object_construct_(size, des);
	invoke4(cons, (int)ptr[Obj_Data], a, b, c);
	if (object_construct_callback) invoke1(object_construct_callback, ptr[Obj_Data]);
	return (int*)ptr[Obj_Data];
}

// Call to construct an object with 'size' elements in the data structure,
// and takes 4 arguments for its constructor.
int *object_construct4 (int size, int *cons, int *des, int a, int b, int c, int d) {
	int *ptr; ptr = object_construct_(size, des);
	invoke5(cons, (int)ptr[Obj_Data], a, b, c, d);
	if (object_construct_callback) invoke1(object_construct_callback, ptr[Obj_Data]);
	return (int*)ptr[Obj_Data];
}

// Call to construct an object with 'size' elements in the data structure,
// and takes 5 arguments for its constructor.
int *object_construct5 (int size, int *cons, int *des, int a, int b, int c, int d, int e) {
	int *ptr; ptr = object_construct_(size, des);
	invoke6(cons, (int)ptr[Obj_Data], a, b, c, d, e);
	if (object_construct_callback) invoke1(object_construct_callback, ptr[Obj_Data]);
	return (int*)ptr[Obj_Data];
}

// Call to construct an object with 'size' elements in the data structure,
// and takes 6 arguments for its constructor.
int *object_construct6 (int size, int *cons, int *des, int a, int b, int c, int d, int e, int f) {
	int *ptr; ptr = object_construct_(size, des);
	invoke7(cons, (int)ptr[Obj_Data], a, b, c, d, e, f);
	if (object_construct_callback) invoke1(object_construct_callback, ptr[Obj_Data]);
	return (int*)ptr[Obj_Data];
}

// Call to construct an object with 'size' elements in the data structure,
// and takes 7 arguments for its constructor.
int *object_construct7 (int size, int *cons, int *des, int a, int b, int c, int d, int e, int f, int g) {
	int *ptr; ptr = object_construct_(size, des);
	invoke8(cons, (int)ptr[Obj_Data], a, b, c, d, e, f, g);
	if (object_construct_callback) invoke1(object_construct_callback, ptr[Obj_Data]);
	return (int*)ptr[Obj_Data];
}

// Call to construct an object with 'size' elements in the data structure,
// and takes 8 arguments for its constructor.
int *object_construct8 (int size, int *cons, int *des, int a, int b, int c, int d, int e, int f, int g, int h) {
	int *ptr; ptr = object_construct_(size, des);
	invoke9(cons, (int)ptr[Obj_Data], a, b, c, d, e, f, g, h);
	if (object_construct_callback) invoke1(object_construct_callback, ptr[Obj_Data]);
	return (int*)ptr[Obj_Data];
}

// Call to construct an object with 'size' elements in the data structure,
// and takes 9 arguments for its constructor.
int *object_construct9 (int size, int *cons, int *des, int a, int b, int c, int d, int e, int f, int g, int h, int i) {
	int *ptr; ptr = object_construct_(size, des);
	invoke10(cons, (int)ptr[Obj_Data], a, b, c, d, e, f, g, h, i);
	if (object_construct_callback) invoke1(object_construct_callback, ptr[Obj_Data]);
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
// Push but check if given item is at the top of the stack.
int *gc_push_checked (int *ptr) {
	if (*gc_ptr != (int)ptr)
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

void gc_construct_collect (int *obj) {
	// Don't use checked version, as this is a newly created object, nobody will have pushed
	// it onto the stack.
	gc_push(obj);
	// For fun, stacktrace every created object
	stacktrace();
}

int gc_autocollect_enabled;
int *gc_enable_autocollect () {
	object_construct_callback = (int*)&gc_construct_collect;
	gc_autocollect_enabled = 1;
	return gc_ptr;
}

void gc_disable_autocollect () {
	object_construct_callback = 0;
	gc_autocollect_enabled = 0;
}

// Enables autocollect, and returns whether it was enabled before
int gc_autocollect_push (int enable) {
	int curr;
	curr = gc_autocollect_enabled;
	if(enable && !curr)
		gc_enable_autocollect();
	else if(!enable && curr)
		gc_disable_autocollect();
	return curr;
}

void gc_autocollect_pop (int enabled) {
	if (!enabled && gc_autocollect_enabled)
		gc_disable_autocollect();
	else if(enabled && !gc_autocollect_enabled)
		gc_enable_autocollect();
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
	
	gc_autocollect_enabled = 0;

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

// placeholder for throw()
// TODO: design for catch/throw/finally:
//   store a catch block address on the stack somewhere.
//   perhaps always reserve and push current value
// catch: look for stack's catch block, rewind stack to that pointer
// for now this placeholder exits
void /* no_return */ throw(char *msg) {
	printf("STUB: throw '%s'\n", msg);
	exit(-1);
}

#endif

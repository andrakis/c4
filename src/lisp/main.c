/*
 * C4 Lisp
 * main.c - Main entry point, and main compile target.
 *
 * Yet another attempt.
 */

#include "lib/dict.c"
#include "lib/vector.c"

// The basic data type, LispObject
enum {
	LO_TYPE,      // See LTYPE_*
	LO_VALUE,     // void *, depends on LO_TYPE
	LO_TAIL,      // Pointer to a tail list if any
	LO__Sz
};

enum {
	LTYPE_NIL,    // A falsey value
	LTYPE_ATOM,   // A string by another name
	LTYPE_INT,    // A number, integer size
	LTYPE_STRING, // A string
	LTYPE_LIST,   // A list, value points to first element, tail to tail
	LTYPE_LAMBDA, // A callable function
	LTYPE_MACRO,  // A macro used in the preprocessing stage
	LTYPE_BUILTIN,// A builtin function to the emulator
};

#define LispObject int

// Configuration
enum {
	LISP_STACK_SIZE = 0x1FF
};

//
// Global data
//

LibVector      *stack;    // Main stack allocation
LibVectorEntry *sp, *bp;  // Pointers within stack allocation

LibVectorEntry *stack_push (int type, int value, int *tail) {
	// --sp = LispObjectData
	sp = libvector_prev(sp, stack);
	sp[LO_TYPE]  = type;
	sp[LO_VALUE] = value;
	sp[LO_TAIL]  = (int)tail;
	return sp;
}

// Builtin functions
LispObject *print (LispObject *params, LibDict *env) {
}


int allocate_data () {
	if (!(stack = libvector_new(LO__Sz * sizeof(int), LISP_STACK_SIZE))) {
		printf("out of memory allocating stack with %ld elements (%ld bytes)\n",
		       LISP_STACK_SIZE, LISP_STACK_SIZE * (LO__Sz * sizeof(int)));
		return 1;
	}
	sp = bp = libvector_prev(libvector_end(stack), stack);
}

void free_data () {
	libvector_free(stack);
}

int main (int argc, char **argv) {
	int i;

	if ((i = allocate_data()))
		return i;

	free_data();
	return 0;
}

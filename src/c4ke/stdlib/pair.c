/**
 * C4 Class: Pair
 * Implements a pair class, with each member having a specified word size.
 *
 */

#ifndef _C4STDLIB_PAIR
#include <stdlib.h>
#include <stdio.h>

#include "classes.c"

/* class pair {
 *     int *loc_b, *memory, size;
 *     pair(int a_size, int b_size, int *a_value, int *b_value) {
 *         memory = malloc(size = (a_size + b_size) * sizeof(int)) || throw(alloc_failure);
 *         loc_b = memory + a_size;
 *         memcpy(memory, a_value, a_size * sizeof(int));
 *         memcpy(memory + a_size, b_value, b_size * sizeof(int));
 *     }
 *     pair(const pair &other) {
 *         // copy other memory up to memory+size
 *     }
 *     ~pair() { free(memory); }
 *     int *first () { return memory; }
 *     int *second () { return loc_b; }
 *     swap(const pair &other) {
 *         swap2(memory, other.memory);
 *         swap2(loc_b,  other.loc_b);
 *     }
 *     // implement copy
 */

 enum {
	/* public */
	C4Pair_First,
	C4Pair_Second,
	C4Pair_swap,
	C4Pair_copy,
	/* private */
	_pair_loc_b, _pair_memory, _pair_size, _pair__structsize
};

int sizeof_C4Pair (int a_size, int b_size) { return _pair__structsize + (a_size + b_size * sizeof(int)); }

/* pair_construct_int_int_int_int */
void pair_construct4 (int *self, int a_size, int b_size, int *a_value, int *b_value) {
	if(!(self[_pair_memory] = (int)malloc(self[_pair_size] = (a_size + b_size) * sizeof(int)))) {
		// TODO: throw
		throw("pair_construct4: malloc fail");
	}
	self[_pair_loc_b] = (int)((int*)self[_pair_memory] + a_size);
	memcpy((void*)self[_pair_memory], a_value, a_size * sizeof(int));
	memcpy((void*)self[_pair_loc_b], b_value, b_size * sizeof(int));
}

void pair_destruct (int *self) {
	free((void*)self[_pair_memory]);
}

int *C4Pair_impl_first (int *self) { return (int*)self[_pair_memory]; }
int *C4Pair_impl_second (int *self) { return (int*)self[_pair_loc_b]; }
void C4Pair_impl_swap (int *self, int *other) {
	swap2(&self[_pair_loc_b], &other[_pair_loc_b]);
	swap2(&self[_pair_memory], &other[_pair_memory]);
	swap2(&self[_pair_size], &other[_pair_size]);
}
void C4Pair_impl_copy (int *self, int *other) {
	// TODO
}

int *pair_new (int a_size, int b_size, int *a_value, int *b_value) {
	int *ptr;
	ptr = object_construct4(_pair__structsize * sizeof(int), (int*)&pair_construct4, (int*)&pair_destruct, a_size, b_size, (int)a_value, (int)b_value);
	ptr[C4Pair_First] = (int)&C4Pair_impl_first;
	ptr[C4Pair_Second] = (int)&C4Pair_impl_second;
	ptr[C4Pair_swap] = (int)&C4Pair_impl_swap;
	ptr[C4Pair_copy] = (int)&C4Pair_impl_copy;
	return ptr;
}

#endif

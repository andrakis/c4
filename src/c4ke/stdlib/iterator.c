// C4 Stdlib Class: Iterator
//
// Implements a standard interface to custom iterators.
//

#ifndef _C4STDLIB_ITERATOR
#define _C4STDLIB_ITERATOR 1

#include "classes.c"
#include "stdlib/lambda.c"

// struct C4Iterator {
//   // should create a new C4Iterator with appropriate index
//   // index of -1 means reached end
//   C4Iterator* (*advance)(C4Iterator *self, int n);
//   int begin(C4Iterator *self) { }
//   int end(C4Iterator *self) { }
//   int prev(C4Iterator *self) { }
//   int next(C4Iterator *self) { }
//   int distance(C4Iterator *self, C4Iterator *first, C4Iterator *last) {
//   }
//   int (*get) (C4Iterator *self); // dereference pointer
//   C4Iterator(int index, int *data, int *get, int *advance) {
//   }
//   ~C4Iterator() { }
//   int  size;  // size of item pointed to
//   protected:
//   void *data;
//   int  index; // most iterators need to track this somehow
// }

enum { C4Iterator_get, C4Iterator_advance,
       C4Iterator_size, _C4Iterator_data, _C4Iterator_index, _C4Iterator__sz };
int sizeof_C4Iterator() { return _C4Iterator__sz * sizeof(int); }

void C4Iterator_construct (int *self, int size, int index, int *data, int *get, int *advance) {
	self[C4Iterator_get] = (int)get;
	self[C4Iterator_advance] = (int)advance;
	self[C4Iterator_size] = size;
	self[_C4Iterator_data] = (int)data;
	self[_C4Iterator_index] = index;
}

void C4Iterator_construct_iterator(int *self, int *iterator, int index) {
	self[C4Iterator_get] = iterator[C4Iterator_get];
	self[C4Iterator_advance] = iterator[C4Iterator_advance];
	self[C4Iterator_size] = iterator[C4Iterator_size];
	self[_C4Iterator_data] = iterator[_C4Iterator_data];
	self[_C4Iterator_index] = index;
}

void C4Iterator_destruct (int *self) {
	printf("C4Iterator_destruct: 0x%X\n", self);
}

int *new_C4Iterator (int size, int index, int *data, int *get, int *advance) {
	int *ptr;
	ptr = object_construct5(_C4Iterator__sz * sizeof(int), (int*)&C4Iterator_construct, (int*)&C4Iterator_destruct,
	                        size, index, (int)data, (int)get, (int)advance);
	return ptr;
}

int *new_C4Iterator_Iterator (int *iterator, int index) {
	int *ptr;
	ptr = object_construct2(_C4Iterator__sz * sizeof(int), (int*)&C4Iterator_construct_iterator, (int*)&C4Iterator_destruct,
	                        (int)iterator, index);
	return ptr;
}

int *C4Iterator_copy (int *iterator) {
	return new_C4Iterator_Iterator(iterator, iterator[_C4Iterator_index]);
}

int *std_next (int *c4iterator)  { return (int*)invoke1((int*)c4iterator[C4Iterator_advance], 1); }
int *std_prev (int *c4iterator)  { return (int*)invoke1((int*)c4iterator[C4Iterator_advance], -1); }
int *std_begin (int *c4iterator) { return new_C4Iterator_Iterator(c4iterator, 0); }
int *std_end (int *c4iterator)   { return new_C4Iterator_Iterator(c4iterator, -1); }
int *std_get (int *c4iterator)   { return (int*)invoke1((int*)c4iterator[C4Iterator_get], (int)c4iterator); }
int std_distance (int *first, int *last) {
	int dist, ge, *gc, *curr, *next; dist = 0;
	if (first[_C4Iterator_index] > last[_C4Iterator_index])
		swap2(first, last);
	curr = C4Iterator_copy(first);
	while(curr[_C4Iterator_index] != -1 && curr[_C4Iterator_index] < last[_C4Iterator_index]) {
		ge = gc_autocollect_push(0);
		next = std_next(curr);
		gc_autocollect_pop(ge);
		object_destruct(curr);
		curr = next;
		++dist;
	}
	object_destruct(curr);
	return dist;
}

void std_foreach (int *c4iterator, int *c4lambda) {
	int *curr, *next, ge1, ge2, *gc;
	ge1 = gc_autocollect_push(0);
	c4iterator = C4Iterator_copy(c4iterator);
	while(c4iterator[_C4Iterator_index] != -1) {
		ge2 = gc_autocollect_push(1);
		gc = gc_ptr;
		invoke1((int*)c4lambda[C4Lambda_Invoke], invoke1((int*)c4iterator[C4Iterator_get], (int)c4iterator));
		gc_collect(gc_ptr - gc);
		gc_autocollect_pop(ge2);
		ge2 = gc_autocollect_push(0);
		next = std_next(c4iterator);
		object_destruct(c4iterator);
		gc_autocollect_pop(ge2);
		c4iterator = next;
	}
	object_destruct(c4iterator);
}

#endif

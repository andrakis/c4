// C4 Multiload Class: Vector
//
// Implements a vector<?>, custom records resizable with optional capacity.

#ifndef _C4STDLIB_VECTOR
#define _C4STDLIB_VECTOR 1

#include <stdlib.h>
//#include "c4.h"

#include "classes.c"
#include "stdlib/iterator.c"

// Constants
enum { _c4vector_alloc = 20 };

// Class Vector {
//   int *vector;
//   int  cap;
//   int  elem_num;
//   int  elem_size;
// }
enum {
	// Iterator
	C4Vector_begin,
	// Vector implementation
	C4Vector_PushBack, C4Vector_PushBackWords, C4Vector_PopBack,
	C4Vector_IncreaseSize, C4Vector_DecreaseSize,
	C4Vector_Print,
	C4Vector_Empty, C4Vector_Size, C4Vector_Capacity,
	C4Vector_IndexOf, C4Vector_AddressOf,
	C4Vector_Swap,
	_c4vector_vector, _c4vector_cap, _c4vector_elem_num, _c4vector_elem_size,
	C4Vector__sz
};

int sizeof_C4Vector (int elem_size) { return C4Vector__sz * elem_size; }

int *C4Vector_fx_begin;
int *C4Vector_fx_PushBack, *C4Vector_fx_PushBackWords, *C4Vector_fx_PopBack;
int *C4Vector_fx_IncreaseSize, *C4Vector_fx_DecreaseSize;
int *C4Vector_fx_Print;
int *C4Vector_fx_Empty, *C4Vector_fx_Size, *C4Vector_fx_Capacity;
int *C4Vector_fx_IndexOf, *C4Vector_fx_AddressOf;
int *C4Vector_fx_Swap;

// Helper: returns the number of bytes needed for a vector
int C4Vector_bytesize (int *self) {
	return sizeof(int) * self[_c4vector_elem_size] * self[_c4vector_cap];
}

// Generic constructor, intended to be called by specific constructors (initializes elements)
void C4Vector_construct_ (int *self) {
	if(self[_c4vector_vector] == 0) {
		// TODO: throw, invalid construction
		printf("C4Vector_construct_: assertion failed, _vector == 0\n");
		exit(-5);
	}
	memset((void*)self[_c4vector_vector], 0, C4Vector_bytesize(self));
}

void C4Vector_construct1 (int *self, int elem_size) {
	printf("C4Vector_construct@%p\n", self);
	self[_c4vector_elem_size] = elem_size;
	self[_c4vector_cap]       = _c4vector_alloc;
	self[_c4vector_vector]    = (int)malloc(C4Vector_bytesize(self));
	self[_c4vector_elem_num]  = 0;
	C4Vector_construct_(self);
}

void C4Vector_construct2 (int *self, int elem_size, int capacity) {
	printf("C4Vector_construct@%p\n", self);
	self[_c4vector_elem_size] = elem_size;
	self[_c4vector_cap]       = capacity;
	self[_c4vector_vector]    = (int)malloc(C4Vector_bytesize(self));
	self[_c4vector_elem_num]  = 0;
	C4Vector_construct_(self);
}

void C4Vector_destruct (int *self) {
	printf("~C4Vector@%p(%p,%d,%d)\n", self, (int*)self[_c4vector_vector], self[_c4vector_cap], self[_c4vector_elem_num]);
	free((int*)self[_c4vector_vector]);
}

void new_C4Vector_fillmembers (int *ptr) {
	ptr[C4Vector_begin]         = (int)C4Vector_fx_begin;
	ptr[C4Vector_PushBack]      = (int)C4Vector_fx_PushBack;
	ptr[C4Vector_PushBackWords] = (int)C4Vector_fx_PushBackWords;
	ptr[C4Vector_PopBack]       = (int)C4Vector_fx_PopBack;
	ptr[C4Vector_IncreaseSize]  = (int)C4Vector_fx_IncreaseSize;
	ptr[C4Vector_DecreaseSize]  = (int)C4Vector_fx_DecreaseSize;
	ptr[C4Vector_Print]   = (int)C4Vector_fx_Print;
	ptr[C4Vector_Empty]   = (int)C4Vector_fx_Empty;
	ptr[C4Vector_Size]    = (int)C4Vector_fx_Size;
	ptr[C4Vector_Capacity]  = (int)C4Vector_fx_Capacity;
	ptr[C4Vector_IndexOf]   = (int)C4Vector_fx_IndexOf;
	ptr[C4Vector_AddressOf] = (int)C4Vector_fx_AddressOf;
	ptr[C4Vector_Swap]    = (int)C4Vector_fx_Swap;
}

int *new_C4Vector1 (int elem_size) {
	int *ptr;
	ptr = object_construct1(C4Vector__sz * sizeof(int), (int*)&C4Vector_construct1, (int*)&C4Vector_destruct, elem_size);
	new_C4Vector_fillmembers(ptr);
	return ptr;
}

int *new_C4Vector2 (int elem_size, int size) {
	int *ptr;
	ptr = object_construct2(C4Vector__sz * sizeof(int), (int*)&C4Vector_construct2, (int*)&C4Vector_destruct, elem_size, size);
	new_C4Vector_fillmembers(ptr);
	return ptr;
}

int c4vector_memcpy (int *src, int *dst, int words) {
	while(words--)
		*dst++ = *src++;
	return 0;
}

// Create a copy of a vector
int *new_C4VectorVector (int *sourceVector) {
	int cap, sz, *ptr, i, *src, *dst;
	cap = sourceVector[_c4vector_cap];
	sz  = sourceVector[_c4vector_elem_size];
	ptr = object_construct2(C4Vector__sz * sizeof(int), (int*)&C4Vector_construct2, (int*)&C4Vector_destruct, sz, cap);
	new_C4Vector_fillmembers(ptr);
	// Copy members
	ptr[_c4vector_cap] = cap;
	ptr[_c4vector_elem_num] = sourceVector[_c4vector_elem_num];
	// Copy elements
	//c4vector_memcpy((int*)sourceVector[_c4vector_vector], (int*)ptr[_c4vector_vector], cap * sz);
	memcpy((void*)ptr[_c4vector_vector], (void*)sourceVector[_c4vector_vector], cap * sz * sizeof(int));
	return ptr;
}

void c4vector_increase_size (int *self, int newcap) {
	void *t;
	int   newbytes, empty_start, toclear;
	t = realloc((void*)self[_c4vector_vector], (newbytes = sizeof(int) * self[_c4vector_elem_size] * newcap));
	if (!t) {
		// TODO: out of memory, throw exception
		printf("C4Vector: realloc failed\n");
		exit(-4);
	}
	empty_start = C4Vector_bytesize(self);
	toclear = newcap > empty_start;
	self[_c4vector_vector] = (int)t;
	self[_c4vector_cap] = newcap;
	if (self[_c4vector_elem_num] >= newcap)
		self[_c4vector_elem_num] = newcap;
	if (toclear) {
		toclear = newbytes - empty_start;
		memset((void*)self[_c4vector_vector], 0, toclear);
	}
}
int c4vector_decrease_size (int *self) {
	int target;

	// Not enough space if we decrease in size
	if (self[_c4vector_elem_num] >= (target = self[_c4vector_cap] / 2))
		return 1;
	//c4vector_realloc(self, target);
	c4vector_increase_size(self, target);
	return 0;
}

void C4Vector_impl_PushBack (int *self, int value) {
	if (self[_c4vector_elem_num] >= self[_c4vector_cap]) {
		c4vector_increase_size(self, self[_c4vector_cap] * 2);
	}
	*((int*)self[_c4vector_vector] + (self[_c4vector_elem_num] * self[_c4vector_elem_size])) = value;
	self[_c4vector_elem_num] = self[_c4vector_elem_num] + 1;
}
void C4Vector_impl_PushBackWords (int *self, int words, int *ptr) {
	if (self[_c4vector_elem_num] >= self[_c4vector_cap]) {
		c4vector_increase_size(self, self[_c4vector_cap] * 2);
	}
	c4vector_memcpy(ptr, (int*)self[_c4vector_vector] + (self[_c4vector_elem_num] * self[_c4vector_elem_size]), words * self[_c4vector_elem_size]);
	self[_c4vector_elem_num] = self[_c4vector_elem_num] + 1;

}
void C4Vector_impl_PopBack (int *self) {
	if (self[_c4vector_elem_num])
		self[_c4vector_elem_num] = self[_c4vector_elem_num] - 1;
}
void C4Vector_impl_IncreaseSize (int *self, int amount) {
	if (amount == 0)
		amount = self[_c4vector_cap];
	c4vector_increase_size(self, amount);
}
int C4Vector_impl_DecreaseSize (int *self) {
	return c4vector_decrease_size(self);
}
void C4Vector_impl_Print (int *self) {
	int i, sz;
	i = 0; sz = self[_c4vector_elem_size];
	printf("C4Vector@%p(%p,%d,%d,%d){ ", self, (int*)self[_c4vector_vector], self[_c4vector_cap], self[_c4vector_elem_num], sz);
	while(i < self[_c4vector_elem_num]) {
		if (i > 0) printf(", ");
		printf("%d", *((int*)self[_c4vector_vector] + (sz * i++)));
	}
	printf(" }");
}
int C4Vector_impl_Empty (int *self) { return self[_c4vector_elem_num] == 0; }
int C4Vector_impl_Size (int *self) { return self[_c4vector_elem_num]; }
int C4Vector_impl_Capacity (int *self) { return self[_c4vector_cap]; }
int C4Vector_impl_IndexOf (int *self, int index) {
	if (index < 0)
		index = self[_c4vector_elem_num] + index;
	if (index > 0 && index <= self[_c4vector_elem_num])
		return *((int*)self[_c4vector_vector] + (self[_c4vector_elem_size] * index));
	printf("Vector: index out of range!\n");
	return 0;
}
int *C4Vector_impl_AddressOf (int *self, int index) {
	if (index < 0)
		index = self[_c4vector_elem_num] + index;
	if (index > 0 && index <= self[_c4vector_elem_num])
		return ((int*)self[_c4vector_vector] + (self[_c4vector_elem_size] * index));
	printf("Vector: index out of range!\n");
	return 0;
}
void C4Vector_impl_Swap (int *self, int *other) {
	swap2(&self[_c4vector_vector], &other[_c4vector_vector]);
	swap2(&self[_c4vector_elem_num], &other[_c4vector_elem_num]);
	swap2(&self[_c4vector_cap], &other[_c4vector_cap]);
	swap2(&self[_c4vector_elem_size], &other[_c4vector_elem_size]);
}

// Iterator implementation
// Section 1: iterator methods
int *C4Vector_iterator_fx_begin, *C4Vector_iterator_fx_get;
int *C4Vector_iterator_impl_get (int *self) {
}
int *C4Vector_iterator_impl_advance (int *self, int z) {
	int v, *x; v = self[_C4Iterator_index] + z;
	x = (int*)self[_C4Iterator_data];
	if (v > x[_c4vector_elem_num])
		v = self[_C4Iterator_index ] = -1; // end()
	return new_C4Iterator_Iterator(self, v);
}
int *C4Vector_iterator_impl_begin (int *self) {
	int *vec; vec = (int*)self[_C4Iterator_data];
	return (int*)invoke1((int*)vec[C4Vector_begin], (int)vec);
}
void C4Vector_iterator_destruct (int *self) {
	printf("C4Vector_iterator_destruct (STUB)\n");
}
//enum { C4Iterator_destruct, C4Iterator_advance, C4Iterator_begin, C4Iterator_end, C4Iterator_prev, C4Iterator_next, C4Iterator_distance,
// Section 2: public interface on vector
int *C4Vector_impl_begin (int *self) {
	return new_C4Iterator(self[_c4vector_elem_size], 0, self, (int*)&C4Vector_iterator_impl_get, (int*)&C4Vector_iterator_impl_advance);
}
int *C4Vector_impl_end (int *self) {
	return new_C4Iterator(self[_c4vector_elem_size], -1, self, (int*)&C4Vector_iterator_impl_get, (int*)&C4Vector_iterator_impl_advance);
}

// Initialization
int C4Vector_init () {
	C4Vector_fx_begin = (int*)&C4Vector_impl_begin;
	C4Vector_fx_PushBack = (int*)&C4Vector_impl_PushBack;
	C4Vector_fx_PushBackWords = (int*)&C4Vector_impl_PushBackWords;
	C4Vector_fx_PopBack = (int*)&C4Vector_impl_PopBack;
	C4Vector_fx_IncreaseSize = (int*)&C4Vector_impl_IncreaseSize;
	C4Vector_fx_DecreaseSize = (int*)&C4Vector_impl_DecreaseSize;
	C4Vector_fx_Empty = (int*)&C4Vector_impl_Empty;
	C4Vector_fx_Print = (int*)&C4Vector_impl_Print;
	C4Vector_fx_Size = (int*)&C4Vector_impl_Size;
	C4Vector_fx_Capacity = (int*)&C4Vector_impl_Capacity;
	C4Vector_fx_IndexOf = (int*)&C4Vector_impl_IndexOf;
	C4Vector_fx_AddressOf = (int*)&C4Vector_impl_AddressOf;
	C4Vector_fx_Swap = (int*)&C4Vector_impl_Swap;
	return 0;
}

int *std_transform (int *c4iterator, int *c4lambda) {
	int *curr, *next, ge1, ge2, *gc, *transform;
	ge1 = gc_autocollect_push(0);
	c4iterator = C4Iterator_copy(c4iterator);
	transform = new_C4Vector1(c4iterator[C4Iterator_size]);
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
	gc_autocollect_pop(ge1);
	object_destruct(c4iterator);
	return transform;
}


#endif

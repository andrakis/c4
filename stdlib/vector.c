// C4 Multiload Class: Vector
//
// Implements a vector<?>, custom records resizable with optional capacity.
#include <stdlib.h>
//#include "c4.h"

#define CLASSES_NOMAIN
#include "classes.c"

// Constants
enum { _c4vector_alloc = 20 };

// Class Vector {
//   int *vector;
//   int  cap;
//   int  elem_num;
//   int  elem_size;
// }
enum {
	_c4vector_vector, _c4vector_cap, _c4vector_elem_num, _c4vector_elem_size,
	C4Vector_PushBack, C4Vector_PushBackWords, C4Vector_PopBack,
	C4Vector_IncreaseSize, C4Vector_DecreaseSize,
	C4Vector_Print,
	C4Vector_Empty, C4Vector_Size, C4Vector_Capacity,
	C4Vector_IndexOf, C4Vector_AddressOf,
	C4Vector_Swap,
	C4Vector__sz
};

int *C4Vector_fx_PushBack, *C4Vector_fx_PushBackWords, *C4Vector_fx_PopBack;
int *C4Vector_fx_IncreaseSize, *C4Vector_fx_DecreaseSize;
int *C4Vector_fx_Print;
int *C4Vector_fx_Empty, *C4Vector_fx_Size, *C4Vector_fx_Capacity;
int *C4Vector_fx_IndexOf, *C4Vector_fx_AddressOf;
int *C4Vector_fx_Swap;

void C4Vector_construct1 (int *self, int elem_size) {
	printf("C4Vector_construct@%p\n", self);
	self[_c4vector_elem_size] = elem_size;
	self[_c4vector_vector]    = (int)malloc(sizeof(int) * elem_size * _c4vector_alloc);
	self[_c4vector_cap]       = _c4vector_alloc;
	self[_c4vector_elem_num]  = 0;
}

void C4Vector_construct2 (int *self, int elem_size, int capacity) {
	printf("C4Vector_construct@%p\n", self);
	self[_c4vector_elem_size] = elem_size;
	self[_c4vector_cap]       = capacity;
	self[_c4vector_vector]    = (int)malloc(sizeof(int) * elem_size * self[_c4vector_cap]);
	self[_c4vector_elem_num]  = 0;
}

void C4Vector_destruct (int *self) {
	printf("~C4Vector@%p(%p,%d,%d)\n", self, (int*)self[_c4vector_vector], self[_c4vector_cap], self[_c4vector_elem_num]);
	free((int*)self[_c4vector_vector]);
}

void new_C4_fillmembers (int *ptr) {
	// These are filled in by C4Vector_init
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
	new_C4_fillmembers(ptr);
	return ptr;
}

int *new_C4Vector2 (int elem_size, int size) {
	int *ptr;
	ptr = object_construct2(C4Vector__sz * sizeof(int), (int*)&C4Vector_construct2, (int*)&C4Vector_destruct, elem_size, size);
	new_C4_fillmembers(ptr);
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
	new_C4_fillmembers(ptr);
	// Copy members
	ptr[_c4vector_cap] = cap;
	ptr[_c4vector_elem_num] = sourceVector[_c4vector_elem_num];
	// Copy elements
	c4vector_memcpy((int*)sourceVector[_c4vector_vector], (int*)ptr[_c4vector_vector], cap * sz);
	return ptr;
}

void c4vector_realloc (int *self, int newcap) {
	int *t, *src, *dst, i, sz;

	// Reallocate
	printf("C4Vector_reallocate(old=%d,new=%d)\n", self[_c4vector_cap], newcap);
	t = malloc(sizeof(int) * self[_c4vector_elem_size] * newcap);
	if (!t) {
		printf("C4Vector: realloc failed\n");
		exit(-3);
	}
	// Copy from _c4vector_vector to t, but not more items than were present
	sz = (newcap > self[_c4vector_cap]) ? self[_c4vector_cap] : newcap;
	c4vector_memcpy((int*)self[_c4vector_vector], t, self[_c4vector_elem_size] * sz);
	self[_c4vector_cap] = newcap;
	free((int*)self[_c4vector_vector]);
	self[_c4vector_vector] = (int)t;
}

void c4vector_increase_size (int *self, int newcap) {
	c4vector_realloc(self, newcap);
}
int c4vector_decrease_size (int *self) {
	int target;

	// Not enough space if we decrease in size
	if (self[_c4vector_elem_num] >= (target = self[_c4vector_cap] / 2))
		return 1;
	c4vector_realloc(self, target);
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
	xorswap(&self[_c4vector_vector], &other[_c4vector_vector]);
	xorswap(&self[_c4vector_elem_num], &other[_c4vector_elem_num]);
	xorswap(&self[_c4vector_cap], &other[_c4vector_cap]);
	xorswap(&self[_c4vector_elem_size], &other[_c4vector_elem_size]);
}

int C4Vector_init () {
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

#ifndef VECTOR_NOMAIN
// test a complex class that has an a and b
enum { testvector_cplx_a, testvector_cplx_b, testvector_cplx_sz };

int main (int argc, char **argv) {
	int i, *v1, *v2, *v3, *s1, *s2, *s3;

	if ((i = classes_init())) return i;
	if ((i = C4Vector_init())) return i;

	v1 = new_C4Vector1(1); s1 = gc_push(v1);
	printf("v1: "); invoke1((int*)v1[C4Vector_Print], (int)v1); printf("\n");
	i = 0;
	while(i < 10) invoke2((int*)v1[C4Vector_PushBack], (int)v1, i++);
	printf("v1: "); invoke1((int*)v1[C4Vector_Print], (int)v1); printf("\n");
	v2 = new_C4VectorVector(v1); gc_push(v2);
	while(i < 20) invoke2((int*)v2[C4Vector_PushBack], (int)v2, i++);
	printf("v2: "); invoke1((int*)v2[C4Vector_Print], (int)v2); printf("\n");

	v3 = new_C4Vector1(testvector_cplx_sz);

	classes_cleanup();
	return 0;
}
#endif

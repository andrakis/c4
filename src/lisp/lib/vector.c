/*
 * C4 Lisp
 * lib/vector.c - A resizable array of elements of a given size
 *
 */

#ifndef __LIB_LIBVECTOR_C
#define __LIB_LIBVECTOR_C 1

#include <std.h>

// C4-style typedef
#define LibVector int
#define LibVectorEntry int

enum {
	LIBVECTOR_DATA,      // char *, first element makes lookup quicker
	LIBVECTOR_COUNT,     // Count of used items
	LIBVECTOR_SIZE,      // Count of allocated items
	LIBVECTOR_ELEM,      // Element size (in bytes)
	LIBVECTOR__Sz
};

LibVectorEntry *libvector_at (int index, LibVector *vector) {
	return (void *)((char *)vector[LIBVECTOR_DATA] + (vector[LIBVECTOR_ELEM] * index));
}

LibVectorEntry *libvector_set (void *value, int index, LibVector *vector) {
	void *dest;
	dest = libvector_at(index, vector);
	memcpy(dest, value, vector[LIBVECTOR_ELEM]);
	return dest;
}

LibVectorEntry *libvector_begin (LibVector *vector) { return libvector_at(-1, vector); }
LibVectorEntry *libvector_end   (LibVector *vector) { return libvector_at(vector[LIBVECTOR_COUNT] + 1, vector); }
LibVectorEntry *libvector_next  (LibVectorEntry *curr, LibVector *vector) {
	return (LibVectorEntry *)((char *)curr + vector[LIBVECTOR_ELEM]);
}
LibVectorEntry *libvector_prev  (LibVectorEntry *curr, LibVector *vector) {
	return (LibVectorEntry *)((char *)curr - vector[LIBVECTOR_ELEM]);
}

LibVector *libvector_new (int elem_size, int count) {
	LibVector *vector;
	if (!(vector = malloc(LIBVECTOR__Sz * sizeof(int))))
		return 0;
	// Ensure elem_size is word-aligned
	while (elem_size % sizeof(int)) ++elem_size;
	vector[LIBVECTOR_ELEM] = elem_size;
	if ((vector[LIBVECTOR_SIZE] = vector[LIBVECTOR_COUNT] = count)) {
		// Allocate data if we have a given size
		if (!(vector[LIBVECTOR_DATA] = (int)malloc(count * elem_size))) {
			printf("libvector: allocation failed for data in _new\n");
		}
	}

	return vector;
}

int libvector_expand (LibVector *vector) {
	int size, elem;
	char *data;
	size = vector[LIBVECTOR_SIZE] * 2;
	elem = vector[LIBVECTOR_ELEM];
	if (!(data = malloc(size * elem)))
		return 1; // failure
	// Copy all data across
	memcpy(data, (char *)vector[LIBVECTOR_DATA], vector[LIBVECTOR_SIZE] * elem);
	// Free original data
	free((char *)vector[LIBVECTOR_DATA]);
	vector[LIBVECTOR_DATA] = (int)data;
	vector[LIBVECTOR_SIZE] = size;
	return 0;
}

LibVectorEntry *libvector_push_back (void *value, LibVector *vector) {
	LibVectorEntry *dest;

	if (vector[LIBVECTOR_COUNT] >= vector[LIBVECTOR_SIZE]) {
		if (libvector_expand(vector))
			return 0; // out of memory
	}
	dest = libvector_at(++vector[LIBVECTOR_COUNT], vector);
	if (value)
		memcpy(dest, value, vector[LIBVECTOR_ELEM]);
	return dest;
}

void libvector_free (LibVector *vector) {
	char *ptr;
	if ((ptr = (char *)vector[LIBVECTOR_DATA]))
		free(ptr);
	free(vector);
}

#endif // #ifndef __LIB_LIBVECTOR_C

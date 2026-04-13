/*
 * C4 Lisp
 * lib/list.c - A doubly linked list holding items of a specified size.
 *
 */

#ifndef __LIB_LIBLIST_C
#define __LIB_LIBLIST_C 1

#include <std.h>

// C4-style typedef
#define LibList int
#define LibListEntry int

// C4-style struct LibListEntry {
enum {
	LIBLIST_ENTRY_PREV,          // LibListEntry *
	LIBLIST_ENTRY_NEXT,          // LibListEntry *
	LIBLIST_ENTRY_VALUE,         // void *, size takes up remaining space
	LIBLIST_ENTRY_LIST,          // LibList *
	LIBLIST_ENTRY__Sz            // size of list element
};
// };

// C4-style struct LibList {
enum {
	LIBLIST_COUNT,           // int, number of items used
	LIBLIST_SIZE,            // int, allocated size
	LIBLIST_ELEM_SIZE,       // const int, size of each element, cannot change
	LIBLIST_ENTRY_SIZE,      // const int, size of each element + entry details
	LIBLIST_DATA,            // char *, allocated data
	LIBLIST__Sz              // size of list structure
};
// };

enum {
	LIBLIST_DEFAULT_SIZE = 2
};

// NOTE: no checking is done on index vs count.
// This is intended, as end iterator is one more than count.
// internal.
static LibListEntry *_liblist_at (int index, LibList *list) {
	LibListEntry *entry;
	char         *data;
	data  = (char *)list[LIBLIST_DATA];
	data  = data + (index * list[LIBLIST_ENTRY_SIZE]);
	entry = (LibListEntry *)data;
	return entry;
}

static LibListEntry *_liblist_iter_add (int amount, LibListEntry *curr) {
	LibList *list;
	list = (int *)curr[LIBLIST_ENTRY_LIST];
	return (LibListEntry *)((char *)curr + (amount * list[LIBLIST_ENTRY_SIZE]));
}

LibListEntry *liblist_begin (LibList *list) { return _liblist_at(-1, list); }
LibListEntry *liblist_end   (LibList *list) { return _liblist_at(list[LIBLIST_COUNT], list); }
LibListEntry *liblist_next  (LibListEntry *curr) { return _liblist_iter_add(1, curr); }
LibListEntry *liblist_prev  (LibListEntry *curr) { return _liblist_iter_add(-1, curr); }

// liblist_reset(LibList list) -> list
// Reset all elements in the linked list, and count to 0.
// Do not call if you need to de-allocate items in the list.
LibList *liblist_reset (LibList *list) {
	char   *data;
	int     elem_size, lentry_size, size;
	LibListEntry *last, *first, *prev, *curr, *next;

	data = (char *)list[LIBLIST_DATA];
	elem_size = list[LIBLIST_ELEM_SIZE];
	lentry_size = list[LIBLIST_ENTRY_SIZE];
	size = list[LIBLIST_SIZE];
	first = (int *)(data);
	last = (int *)(data + (lentry_size * size));
	next = (int *)(data + lentry_size);
	curr = first;
	prev = last;
	while(curr <= last) {
		curr[LIBLIST_ENTRY_PREV] = (int)prev;
		curr[LIBLIST_ENTRY_NEXT] = (int)next;
		curr[LIBLIST_ENTRY_LIST] = (int)list;
		memset(&curr[LIBLIST_ENTRY_VALUE], 0, elem_size);
		prev = curr;
		curr = (int *)((char *)curr + lentry_size);
		next = (int *)((char *)curr + lentry_size);
	}

	// Fixup pointers on first and last elements
	first[LIBLIST_ENTRY_PREV] = (int)last;
	last[LIBLIST_ENTRY_NEXT]  = (int)next;

	return list;
}

LibList *liblist_new (int elem_size, int count) {
	LibList *list;
	char *data;
	int   size, i;

	if (!(size = count))
		size = LIBLIST_DEFAULT_SIZE;
	if (!(list = malloc((i = sizeof(int) * LIBLIST__Sz)))) {
		printf("list.c: out of memory attempting to list of %ld bytes (%ld * %ld)\n", i, sizeof(int), LIBLIST__Sz);
		return 0;
	}
	// Fixup element size to fit word
	while (elem_size % sizeof(int)) ++elem_size;
	list[LIBLIST_ELEM_SIZE] = elem_size;
	list[LIBLIST_ENTRY_SIZE] = elem_size + (sizeof(int) * LIBLIST_ENTRY__Sz);
	if (!(data = (char *)(list[LIBLIST_DATA] = (int)malloc((i = (elem_size + (LIBLIST_ENTRY__Sz * sizeof(int))) * size))))) {
		free(list);
		printf("list.c: out of memory attempting to list data of %ld bytes (%ld * %ld)\n", i, elem_size + (LIBLIST_ENTRY__Sz * sizeof(int)), size);
		return 0;
	}
	list[LIBLIST_SIZE]  = size;
	list[LIBLIST_COUNT] = count;
	return liblist_reset(list);
}

#endif // #ifndef __LIB_LIBLIST_C

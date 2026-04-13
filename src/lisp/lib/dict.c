/*
 * C4 Lisp
 * lib/dict.c - A keyed dictionary. The only supported key type is int.
 *              Lookups are slow in this implementation.
 *
 */

#ifndef __LIB_DICT_C
#define __LIB_DICT_C 1

#include <std.h>

// Uses lib/vector internally.
#include "./vector.c"

#define LibDict int
#define LibDictEntry int

enum {
	LIBDICT_KEY,      // Key on the item
	LIBDICT_VALUE,    // Pointer to value of the item
	LIBDICT__Sz
};

enum { LIBDICT_DEFAULT_SIZE = 4 };

LibDict *libdict_new () {
	LibDict *dict;
	if (!(dict = libvector_new(LIBDICT__Sz * sizeof(int), LIBDICT_DEFAULT_SIZE)))
		return 0;
	// Elements should be empty
	dict[LIBVECTOR_COUNT] = 0;
	return dict;
}

LibDictEntry *libdict_begin (LibDict *dict) { return (LibDictEntry *)libvector_begin(dict); }
LibDictEntry *libdict_end   (LibDict *dict) { return (LibDictEntry *)libvector_end(dict); }
LibDictEntry *libdict_next  (LibDictEntry *curr, LibDict *dict) {
	return (LibDictEntry *)libvector_next(curr, dict);
}
LibDictEntry *libdict_prev  (LibDictEntry *curr, LibDict *dict) {
	return (LibDictEntry *)libvector_prev(curr, dict);
}

LibDictEntry *libdict_find_entry_by_key (int key, LibDict *dict) {
	LibDictEntry *begin, *curr, *end;
	begin = libdict_begin(dict);
	end   = libdict_end(dict);
	curr  = libdict_next(begin, dict);
	while(curr != end) {
		if (curr[LIBDICT_KEY] == key)
			return curr;
	}
	return 0;
}

LibDictEntry *libdict_find_value_by_key (int key, LibDict *dict) {
	LibDictEntry *result;
	if ((result = libdict_find_entry_by_key(key, dict)))
		return (LibDictEntry *)result[LIBDICT_VALUE];
	return 0;
}

LibDictEntry *libdict_set_by_key (int key, int value, LibDict *dict) {
	LibDictEntry *entry;
	if (!(entry = libdict_find_entry_by_key(key, dict))) {
		// Don't copy any data, just allocate
		entry = (LibDictEntry *)libvector_push_back(0, dict);
	}
	entry[LIBDICT_KEY] = key;
	entry[LIBDICT_VALUE] = value;
	return entry;
}

#endif // #ifndef __LIB_DICT_C

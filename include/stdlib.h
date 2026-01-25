//
// C4KE Standard Library: stdlib.h
//

#ifndef __STDLIB_H
#define __STDLIB_H 1

#ifndef __c4cc__
#include_next <stddef.h>
#include_next <stdlib.h>
#else /* ifndef __c4cc__ */

#ifndef C4KE
#include <c4ke/common.h>
#include <stddef.h>

static int OP_STDLIB_MALLOC, OP_STDLIB_FREE;

#if STDLIB_EXPERIMENTAL
#if NO_INLINE
void *malloc (int size) { __c4_opcode(size, OP_STDLIB_MALLOC); }
void  free   (void *ptr){ __c4_opcode(ptr, OP_STDLIB_FREE); }
#else /* if NO_INLINE */
#define malloc(size)             __c4_opcode(size, OP_STDLIB_MALLOC)
#define free(ptr)                __c4_opcode(ptr, OP_STDLIB_FREE)
#endif /* if NO_INLINE */
#endif /* if STDLIB_EXPERIMENTAL */

void *calloc (size_t nmemb, size_t size) {
	int total_sz, new_sz;
	void *ptr;

	// Make sure we don't overflow
	total_sz = 0;
	while (nmemb) {
		if ((new_sz = total_sz + size) < 0 || new_sz < total_sz) {
			// Overflow;
			return 0;
		}
		total_sz = new_sz;
		--nmemb;
	}

	if ((ptr = malloc(total_sz))) {
		memset(ptr, total_sz, 0);
	}

	return ptr;
}

#if STDLIB_EXPERIMENTAL
static int __attribute__((constructor)) __stdlib_init (int *c4r) {
	OP_STDLIB_MALLOC = c4ke_opcode("OP_STDLIB_MALLOC");
	OP_STDLIB_FREE   = c4ke_opcode("OP_STDLIB_FREE");
	return 0;
}
#endif /* #if STDLIB_EXPERIMENTAL */
#endif /* ifndef C4KE */
#endif /* ifndef/else __c4cc__ */
#endif /* ifndef __STDLIB_H */

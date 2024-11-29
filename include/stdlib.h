//
// C4PRE Sample: stdlib.h
// Here would be stdlib.
//

#ifndef __STDLIB_H
#define __STDLIB_H              // Don't use a character

#ifndef C4CC
#include </usr/include/stdlib.h>
#endif

// Rename to prevent gcc warnings
#define malloc c4cc_malloc
void *malloc (int s);
#undef malloc

#endif

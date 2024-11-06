//
// C4PRE Sample: stdlib.h
// Here would be stdlib.
//

#ifndef __STDLIB_H
#define __STDLIB_H              // Don't use a character

// Rename to prevent gcc warnings
#define malloc renamed_malloc
void *malloc (int s);
#undef malloc

#endif

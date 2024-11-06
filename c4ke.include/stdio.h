//
// C4PRE Sample: stdio.h
// Here would be stdio.
//

#ifndef __STDIO_H
#define __STDIO_H 1

#ifndef C4CC
// Rename to prevent gcc warnings
#define printf renamed_printf
int printf(char* fmt, ...);
#undef printf
#endif

#endif

/*
 * C4 Lisp
 * std.h - Standard definitions, should be included first by header files.
 *
 */

#ifndef __STD_H
#define __STD_H 1

#ifndef __c4cc__
// Standard headers, for editor compatibility
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <memory.h>
#include <fcntl.h>

// Silence warnings about formatting
#pragma GCC diagnostic ignored "-Wformat"
#pragma GCC diagnostic ignored "-Wformat-security"
#pragma GCC diagnostic ignored "-Wunused-result"

#ifdef __GNUC__
#include <unistd.h>
#else
#if _WIN64
#define __INTPTR_TYPE__ long long
#elif _WIN32
#define __INTPTR_TYPE__ int
#endif // if _WIN64
#endif // ifdef __GNUC__

// Redefine the int type
#define int __INTPTR_TYPE__

// GCC doesn't like our attributes
#pragma GCC diagnostic ignored "-Wattributes"

#else  // #ifndef __c4cc

// Our own versions of stdlib stuff C4 doesn't include
void *memcpy(void *dst, void *src, int len) {
	int *di, *si, i, max;
	char *dc, *sc;

	i = 0;
	if ((int)dst % sizeof(int) == 0 &&
	    (int)src % sizeof(int) == 0 &&
	    len % sizeof(int) == 0) {
		// Word copy
		di = (int*)dst; si = (int*)src; i = 0;
		max = len / sizeof(int);
		while(i++ < max)
			di[i] = si[i];
	} else {
		// Byte copy
		dc = (char*)dst; sc = (char*)src;
		while(i++ < len)
			dc[i] = sc[i];
	}
	return dst;
}

#endif // #ifndef/else __c4cc__

#define C4R_CONSTRUCTOR(name,c4r) static void __attribute__((constructor)) name (int *c4r)
#define C4R_DESTRUCTOR(name)      static void __attribute__((destructor))  name ()

#endif // #ifndef __STD_H

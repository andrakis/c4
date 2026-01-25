///
// c4lm.h - Main header file for C4 Lightweight Microkernel.
///

#ifndef __C4LM_H
#define __C4LM_H 1

#ifdef PURE_C4
// Disable keywords C4 doesn't know about, when not compiling under c4cc.
#define __attribute__(x)
#define static
#endif

// C4 and C4CC don't know about const
#define const

// Must be first include
#include <stddef.h>

#include <fcntl.h>
#include <unistd.h>

// Implement standard library functions
int __time () {
	// TODO
	return 0;
}

#endif // #ifndef __C4LM_H

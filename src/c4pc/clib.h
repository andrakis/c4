/* clib.h -- the C library calls c4pc makes. On Linux they are libc's; on
 * the bare PC (C4PC_BARE) pc/libc.c has its own, small, under the same
 * names, since gcc emits calls to memcpy and memset of its own accord. */
#ifndef C4PC_CLIB_H
#define C4PC_CLIB_H
#include <stddef.h>
#ifdef C4PC_BARE
void *memcpy(void *d, const void *s, size_t n);
void *memmove(void *d, const void *s, size_t n);
void *memset(void *d, int c, size_t n);
int memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);
int strcmp(const char *a, const char *b);
int strncmp(const char *a, const char *b, size_t n);
char *strchr(const char *s, int c);
char *strrchr(const char *s, int c);
void *malloc(size_t n);
void *calloc(size_t n, size_t k);
void *realloc(void *p, size_t n);
void free(void *p);
void qsort(void *base, size_t n, size_t sz, int (*cmp)(const void *, const void *));
/* the MMU's way out of a fault (mmu.c): pc/boot.S's, which keeps what
 * the i386 calling convention says a callee keeps, and the stack and pc */
typedef unsigned long c4_jmp_buf[8];     /* i386: 6 words; x86-64: 8 */
int c4_setjmp(c4_jmp_buf b) __attribute__((returns_twice));
void c4_longjmp(c4_jmp_buf b, int v) __attribute__((noreturn));
#else
#include <string.h>
#include <stdlib.h>
#include <setjmp.h>
/* without the signal mask, which nothing here changes */
typedef jmp_buf c4_jmp_buf;
#define c4_setjmp(b) _setjmp(b)
#define c4_longjmp(b, v) _longjmp(b, v)
#endif
#endif

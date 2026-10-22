// bareu.c -- the rest of libc4ix that a program linked with libc and
// bare.c needs, with no C4IX under it: varargs, and the few libc4ix calls
// libarc's archive.c makes (ualloc, uprintf, ufprintf), over libc's own.
// Compiled with -fno-builtin, as libc is, so malloc here is libc's, and
// what ualloc gives, libc's free takes back.

#include "libc.h"

// ---- varargs: libc4ix's, as c4lc's variadic calls expect them ----
//
// c4lc gathers a variadic call's arguments with __c4cc_make_va, on a stack
// of its own, and va_end gives them back (__c4ix_uva_adj). A stack per
// program: on the bare machine and under C4DOS there is one task.
enum { UVA_STACK = 128 };
static int *uva_stack;
static int  uva_vptr;
static int *uva_m_ptr;
static int *uva_m_arg;
static int  uva_m_n;

int *__c4cc_make_va(int count) {
    // &count is a fixed frame slot, so the call below cannot disturb
    // the argument walk that follows it.
    if (!uva_stack) uva_stack = (int *)malloc(sizeof(int) * UVA_STACK);
    uva_m_n   = count;
    uva_m_arg = &count + count;
    uva_m_ptr = &uva_stack[uva_vptr];

    uva_stack[uva_vptr] = count; ++uva_vptr;
    while (uva_m_n--) {
        uva_stack[uva_vptr] = *uva_m_arg; ++uva_vptr;
        --uva_m_arg;
    }
    return uva_m_ptr;
}

void __c4ix_uva_adj(int n) {
    uva_vptr = uva_vptr - n;
}

// ---- what archive.c asks of libc4ix ----
int *ualloc(int bytes) { return (int *)malloc(bytes); }

int uprintf(char *fmt, ...) {
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = vprintf(fmt, ap);
    va_end(ap);
    return n;
}

int ufprintf(int fd, char *fmt, ...) {
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = vfprintf(fd == 2 ? stderr : stdout, fmt, ap);
    va_end(ap);
    return n;
}

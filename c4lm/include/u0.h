/**
 * u0.h - The user interface to the C4LM syscalls.
 */
#ifndef __C4LM_U0_H
#define __C4LM_U0_H 1

#include <c4r.h>
#include <c4lm/syscall.h>

int *__u0_c4r, *__u0_syscall;
C4R_CONSTRUCTOR(syscall_constructor, int *c4r, int *syscall) {
	__u0_c4r = c4r;
	__u0_syscall = syscall;
}

// TODO:
// The first time any of these syscalls are used, the computed value of their
// address is saved so that it doesn't have to be calculated every time.
int schedule () {
	int *addr, *dest;

	addr = (int *)&schedule;
	dest = *(__u0_syscall + SYS0_SCHEDULE);
	*addr++ = JMP;
	*addr++ = (int)dest;

	return c4_invoke0(dest);
}

int __time () {
	return c4_invoke0(*(__u0_syscall + SYS0_TIME));
}

void exit (int code) {
	return c4_invoke1(*(__u0_syscall + SYS1_EXIT), code);
}

#endif // #ifndef __C4LM_U0_H

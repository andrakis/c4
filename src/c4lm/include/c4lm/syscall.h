/**
 * c4lm/syscall.h - The system call interface
 *
 */
#ifndef __C4LM_SYSCALL_H
#define __C4LM_SYSCALL_H 1

enum {
	///
	// Syscalls that take no arguments
	///

	// pid():      get the current process id
	SYS0_PID,

	// schedule(): attempt to switch tasks
	// returns:    1 on success, 0 on no task to switch to
	SYS0_SCHEDULE,
	// __time():   get a system timestamp.
	// note: only available timesource in C4 is /proc/uptime.
	SYS0_TIME,

	///
	// Syscalls that take 1 argument
	///
	SYS1_EXIT,

	///
	// End of syscall interface
	///
	SYS__Sz
};

#endif // ifndef __C4LM_SYSCALL_H

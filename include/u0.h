// u0.c - C4KE user runtime
// Intended to be compiled with user applications.
//
// - Overrides calls to various system functions like exit so that a
//   C4KE-friendly variant can run instead.
//   See exit() in this file.
// - Discovers opcodes by requesting them from the kernel.
// - C-friendly functions for using the new opcodes.
//
// In the future this will become a library and header file, but for
// now C4CC does not have a preprocessor. It would be better to have
// a header file define the interface instead of requesting opcodes
// from the kernel.

#ifndef __U0_C
#define __U0_C

#include "c4.h"
#include "c4m.h"

enum { U0_DEBUG = 0 };

// C4KE opcode: int request_opcode(char *name)
enum { OP_REQUEST_SYMBOL = 128 };

// Task priveleges
enum { PRIV_NONE, PRIV_USER, PRIV_KERNEL };

// Kernel task information
enum {
	KTI_COUNT,
	KTI_USED,
	KTI_LIST,  // see KTE_*
	KTI__Sz
};

// Kernel task element
enum {
	// First element, so can be referenced as *kte instead of kte[KTE_TASK_STATE]
	KTE_TASK_STATE,    // int
	KTE_TASK_ID,       // int
	KTE_TASK_PARENT,   // int
	KTE_TASK_NAME,     // char *
	KTE_TASK_NAMELEN,  // int
	KTE_TASK_PRIORITY, // int
	KTE_TASK_PRIVS,    // int, PRIV_*
	KTE_TASK_NICE,     // int
	KTE_TASK_CYCLES,   // int
	KTE_TASK_TIMEMS,   // int
	KTE_TASK_TRAPS,    // int
	KTE__Sz
};

// Task state, keep up to date with c4ke.c
enum {
	STATE_UNLOADED = 0x0,
	STATE_LOADED   = 0x1,
	STATE_RUNNING  = 0x2,
	STATE_WAITING  = 0x4,
	STATE_TRAPPED  = 0x8,
	STATE_ETHEREAL = 0x10, // Not a real task, used by kernel process
	STATE_ZOMBIE   = 0x20, // Waiting to die
};

// __c4_info() flags

// C4INFO state
enum {
	C4I_NONE = 0x0,  // No C4 info
	C4I_C4   = 0x1,  // Ultimately running under C4
	C4I_C4M  = 0x2,  // Running under c4m (directly or C4)
	C4I_C4P  = 0x4,  // Running under c4plus
	C4I_HRT  = 0x10, // High resolution timer
	C4I_SIG  = 0x20, // Signals supported
	C4I_C4KE = 0x40, // C4KE is running
};

// TODO: make a c4cc header file
// Trap codes
enum {
	// Illegal opcode, allows custom opcodes to be implemented using
	// install_trap_handler()
	TRAP_ILLOP,
	// Hard IRQ generated by c4m under some condition
	TRAP_HARD_IRQ,
	// Soft IRQ generated by user code
	TRAP_SOFT_IRQ,
	// A POSIX signal was received
	TRAP_SIGNAL,
	// Invalid memory read or write
	TRAP_SEGV,
	// Invalid opcode value (specifically with OPCD)
	TRAP_OPV,
};
// TRAP_HARD_IRQ codes
enum {
	HIRQ_CYCLE       // Cycle, runs every X cycles
};

#ifndef C4CC
#undef __c4_trap
#define __c4_trap(a,b)
#endif

// Signals
// Stop regular compilers from caring about these signals
#undef SIGHUP
#undef SIGINT
#undef SIGQUIT
#undef SIGILL
#undef SIGTRAP
#undef SIGABRT
#undef SIGBUS
#undef SIGFPE
#undef SIGKILL
#undef SIGUSR1
#undef SIGSEGV
#undef SIGUSR2
#undef SIGPIPE
#undef SIGALRM
#undef SIGTERM
#undef SIGSTKFLT
#undef SIGCHLD
#undef SIGCONT
#undef SIGSTOP
#undef SIGTSTP
#undef SIGTTIN
#undef SIGTTOU
#undef SIGURG
#undef SIGXCPU
#undef SIGXFSZ
#undef SIGVTALRM
#undef SIGPROF
#undef SIGWINCH
#undef SIGIO
#undef SIGPWR
#undef SIGSYS
#undef SIGRTMIN
#undef SIGRTMAX
enum {
  SIGHUP = 1  ,SIGINT      ,SIGQUIT     ,SIGILL      ,SIGTRAP,
  SIGABRT     ,SIGBUS      ,SIGFPE      ,SIGKILL     ,SIGUSR1,
  SIGSEGV     ,SIGUSR2     ,SIGPIPE     ,SIGALRM     ,SIGTERM,
  SIGSTKFLT   ,SIGCHLD     ,SIGCONT     ,SIGSTOP     ,SIGTSTP,
  SIGTTIN     ,SIGTTOU     ,SIGURG      ,SIGXCPU     ,SIGXFSZ,
  SIGVTALRM   ,SIGPROF     ,SIGWINCH    ,SIGIO       ,SIGPWR,
  SIGSYS      ,SIGRTMIN = 32,SIGRTMAX = 64, SIGMAX = 64
};

///
/// Custom opcodes discovery and helper functions
///

// Filled out during startup
static int OP_HALT, OP_C4INFO, OP_TIME;
static int OP_SCHEDULE, OP_AWAIT_MESSAGE, OP_AWAIT_PID, OP_USER_START_C4R;
static int OP_KERN_TASKS_EXPORT, OP_KERN_TASKS_EXPORT_FREE, OP_KERN_TASKS_EXPORT_UPDATE;
static int OP_KERN_TASKS_RUNNING;
static int OP_KERN_TASK_CURRENT_ID, OP_KERN_TASK_RUNNING;
static int OP_KERN_TASK_COUNT;
static int OP_TASK_FINISH, OP_TASK_EXIT, OP_TASK_FOCUS, OP_TASK_CYCLES;
static int OP_USER_SLEEP, OP_USER_PID, OP_USER_PARENT, OP_USER_SIGNAL, OP_USER_KILL;
static int OP_CURRENTTASK_UPDATE_NAME;
static int OP_DEBUG_KERNELSTATE;
static int OP_KERN_REQUEST_EXCLUSIVE, OP_KERN_RELEASE_EXCLUSIVE;

// Automatically initialize opcodes
static int __u0_ops_init () {
	if (U0_DEBUG) printf("u0: ops_init()\n");
	// These calls must be made in reverse order due to how arguments are pushed
	OP_HALT = __c4_opcode("OP_HALT", OP_REQUEST_SYMBOL);
	OP_C4INFO = __c4_opcode("OP_C4INFO", OP_REQUEST_SYMBOL);
	OP_TIME = __c4_opcode("OP_TIME", OP_REQUEST_SYMBOL);
	OP_SCHEDULE = __c4_opcode("OP_SCHEDULE", OP_REQUEST_SYMBOL);
	OP_AWAIT_MESSAGE = __c4_opcode("OP_AWAIT_MESSAGE", OP_REQUEST_SYMBOL);
	OP_AWAIT_PID = __c4_opcode("OP_AWAIT_PID", OP_REQUEST_SYMBOL);
	//printf("Got for op schedule: %d\n", OP_SCHEDULE);
	OP_KERN_TASKS_EXPORT = __c4_opcode("OP_KERN_TASKS_EXPORT", OP_REQUEST_SYMBOL);
	OP_KERN_TASKS_EXPORT_UPDATE = __c4_opcode("OP_KERN_TASKS_EXPORT_UPDATE", OP_REQUEST_SYMBOL);
	OP_KERN_TASKS_EXPORT_FREE = __c4_opcode("OP_KERN_TASKS_EXPORT_FREE", OP_REQUEST_SYMBOL);
	OP_KERN_TASKS_RUNNING = __c4_opcode("OP_KERN_TASKS_RUNNING", OP_REQUEST_SYMBOL);
	OP_USER_START_C4R = __c4_opcode("OP_USER_START_C4R", OP_REQUEST_SYMBOL);
	OP_KERN_TASK_CURRENT_ID = __c4_opcode("OP_KERN_TASK_CURRENT_ID", OP_REQUEST_SYMBOL);
	OP_KERN_TASK_RUNNING = __c4_opcode("OP_KERN_TASK_RUNNING", OP_REQUEST_SYMBOL);
	OP_KERN_TASK_COUNT = __c4_opcode("OP_KERN_TASK_COUNT", OP_REQUEST_SYMBOL);
	OP_TASK_FINISH = __c4_opcode("OP_TASK_FINISH", OP_REQUEST_SYMBOL);
	OP_TASK_FOCUS = __c4_opcode("OP_TASK_FOCUS", OP_REQUEST_SYMBOL);
	OP_TASK_EXIT = __c4_opcode("OP_TASK_EXIT", OP_REQUEST_SYMBOL);
	OP_USER_SIGNAL = __c4_opcode("OP_USER_SIGNAL", OP_REQUEST_SYMBOL);
	OP_USER_KILL = __c4_opcode("OP_USER_KILL", OP_REQUEST_SYMBOL);
	OP_USER_SLEEP = __c4_opcode("OP_USER_SLEEP", OP_REQUEST_SYMBOL);
	OP_USER_PID = __c4_opcode("OP_USER_PID", OP_REQUEST_SYMBOL);
	OP_USER_PARENT = __c4_opcode("OP_USER_PARENT", OP_REQUEST_SYMBOL);
	OP_CURRENTTASK_UPDATE_NAME = __c4_opcode("OP_CURRENTTASK_UPDATE_NAME", OP_REQUEST_SYMBOL);
	OP_DEBUG_KERNELSTATE = __c4_opcode("OP_DEBUG_KERNELSTATE", OP_REQUEST_SYMBOL);
	OP_KERN_REQUEST_EXCLUSIVE = __c4_opcode("OP_KERN_REQUEST_EXCLUSIVE", OP_REQUEST_SYMBOL);
	OP_KERN_RELEASE_EXCLUSIVE = __c4_opcode("OP_KERN_RELEASE_EXCLUSIVE", OP_REQUEST_SYMBOL);
	OP_TASK_CYCLES = __c4_opcode("OP_TASK_CYCLES", OP_REQUEST_SYMBOL);
    if (U0_DEBUG) printf("u0: ~ops_init()\n");
	return 0;
}

// These calls must be made in reverse order due to how arguments are pushed
int schedule () { return __c4_opcode(OP_SCHEDULE); }
int *kern_tasks_export () { return (int *)__c4_opcode(OP_KERN_TASKS_EXPORT); }
void kern_tasks_export_update (int *kti) { __c4_opcode(kti, OP_KERN_TASKS_EXPORT_UPDATE); }
void kern_tasks_export_free (int *kti) { __c4_opcode(kti, OP_KERN_TASKS_EXPORT_FREE); }
int  kern_tasks_running () { return 0; } // TODO
int  kern_user_start_c4r (int argc, char **argv, char *name, int privileges) {
	// printf("u0: attempt to start c4r\n");
	return __c4_opcode(privileges, name, argv, argc, OP_USER_START_C4R);
}
int kern_request_exclusive () { return __c4_opcode(OP_KERN_REQUEST_EXCLUSIVE); }
int kern_release_exclusive () { return __c4_opcode(OP_KERN_RELEASE_EXCLUSIVE); }

void __print_task_state (int s) {
	//if(s & STATE_LOADED)  { printf("L"); }
	if(s & STATE_ZOMBIE)       printf("Z");
	else if(s & STATE_WAITING) printf("W");
	else if(s & STATE_TRAPPED) printf("T");
	else if(s & STATE_RUNNING) printf("R");
	else                       printf("U");
}

// No longer a kernel call
void kern_print_task_state (int state) { __print_task_state(state); }

int __getlen_task_state (int s) {
	return 1; // No longer multichar, always just 1 character
}
// No longer a kernel call
int  kern_getlen_task_state (int state) { return __getlen_task_state(state); }
int  kern_task_current_id () { return __c4_opcode(OP_KERN_TASK_CURRENT_ID); }
int  kern_task_running (int pid) { return __c4_opcode(pid, OP_KERN_TASK_RUNNING); }
void debug_kernelstate () { __c4_opcode(OP_DEBUG_KERNELSTATE); }

///
/// Atexit implementation
///
static int *__u0_atexit_entries, __u0_atexit_count, __u0_atexit_max;

// Make GCC not complain about this function
#define atexit not_an_atexit
int atexit (int *fn) {
	if (__u0_atexit_count == __u0_atexit_max) {
		printf("u0: atexit failure, already at max count\n");
		return 1;
	}
	__u0_atexit_entries[__u0_atexit_count++] = (int)fn;
	return 0;
}

// Atexit initialization
static int __u0_atexit_init () {
	int sz;
	// printf("u0: atexit init\n");
	__u0_atexit_count = 0;
	__u0_atexit_max   = 32;
	if (!(__u0_atexit_entries = malloc((sz = sizeof(int) * __u0_atexit_max)))) {
		printf("u0: failed to allocate atexit entries\n");
		return 1;
	}
	memset(__u0_atexit_entries, 0, sz);
	return 0;
}

// Destructor to run the atexit entries
static int __attribute__((destructor)) __u0_atexit_destruct () {
	int i, *e;
	if (__u0_atexit_count) {
		// printf("u0: running %d atexit entries\n", __u0_atexit_count);
		i = 0;
		while (i < __u0_atexit_count) {
			e = (int *)__u0_atexit_entries[i++];
#define e() ((void (*)(void))e)()
			e();
#undef e
		}
	}
	// printf("u0: freeing atexit memory\n");
	free(__u0_atexit_entries);
}

///
/// Builtin overrides
///

// Convince GCC not to care about this function
#define exit __c4_exit
void exit (int code) {
	// printf("u0: exit called with code %d\n", code);
	__c4_opcode(code, OP_TASK_EXIT);
}

///
/// Utility functions
/// These implement functions in the standard library.
/// Because the signatures do not match the standard library, we must
/// use macro hackery to rename them to avoid errors.
/// We also include some standard headers that define these so that editor-time
/// checking doesn't complain.
///

#define strlen __c4_strlen
static int strlen (char *s) { char *t; t = s; while(*t) ++t; return t - s; }
#define strcmp __c4_strmcp
static int strcmp (char *s1, char *s2) { while(*s1 && (*s1 == *s2)) { ++s1; ++s2; } return *s1 - *s2; }
#define atoi __c4_atoi
static int atoi (char *str, int radix) {
	int v, sign;

	v = 0;
	sign = 1;
	if(*str == '-') {
		sign = -1;
		++str;
	}
	while (
		(*str >= 'A' && *str <= 'Z') ||
		(*str >= 'a' && *str <= 'z') ||
		(*str >= '0' && *str <= '9')) {
		v = v * radix + ((*str > '9') ? (*str & ~0x20) - 'A' + 10 : (*str - '0'));
		++str;
	}
	return v * sign;
}

#define sleep __c4_sleep
int sleep (int ms) {
	// printf("u0: sleep for %dms\n", ms);
	__c4_opcode(ms, OP_USER_SLEEP);
}

enum { TIMEOUT_NEVER = 0 };
int *await_message (int timeout) {
	return __c4_opcode(timeout, OP_AWAIT_MESSAGE);
}

int await_pid (int pid) {
	return __c4_opcode(pid, OP_AWAIT_PID);
}

// Cache the pid and parent id, it presently cannot change
static int  __u0_pid;
static int  __u0_parent;
static int *__u0_c4r; // Could be useful for stack traces
int pid () { return __u0_pid; }       // set by constructor
int parent () { return __u0_parent; } // ""

int itoa_len (int n) {
	int len;
	len = 1;
	while(n / 10 > 0) {
		++len;
		n = n / 10;
	}
	return len;
}

void currenttask_update_name (char *name) {
	__c4_opcode((int)name, OP_CURRENTTASK_UPDATE_NAME);
}

int kern_task_count () {
	return __c4_opcode(OP_KERN_TASK_COUNT);
}

// Instruct the kernel which task has focus
void c4ke_set_focus (int pid) {
	__c4_opcode(pid, OP_TASK_FOCUS);
}

///
/// Signals
///
#define signal __u0_signal
int signal (int sig, int *handler) {
	//printf("u0: signal(%d, 0x%x)\n", sig, handler);
	return __c4_opcode(handler, sig, OP_USER_SIGNAL);
}
// Default handlers
static void default_sighup (int sig) {
	printf("u0: SIGHUP on pid %d!\n", pid());
	exit(-1);
}
static void default_sigint (int sig) {
	printf("u0: SIGINT on pid %d! Exiting.\n", pid());
	exit(-1);
}
static void default_sigquit (int sig) {
	printf("u0: SIGQUIT on pid %d! Quitting.\n", pid());
	exit(-2);
}
static void default_sigill (int sig) {
	printf("u0: SIGILL on pid %d! Stopping.\n", pid());
	exit(-3);
}
static void default_sigtrap (int sig) {
	printf("u0: SIGTRAP on pid %d!\n", pid());
}
static void default_sigabrt (int sig) {
	printf("u0: SIGABRT on pid %d! Aborting.\n", pid());
	exit(-4);
}
static void default_sigterm (int sig) {
	printf("u0: SIGTERM on pid %d! Aborting.\n", pid());
	exit(-5);
}
static void default_sigkill (int sig) {
	printf("u0: SIGKILL on pid %d! Aborting.\n", pid());
	exit(-6);
}
#define kill __u0_kill
static int kill (int pid, int sig) {
	return __c4_opcode(sig, pid, OP_USER_KILL);
}

///
/// Atoi
///
enum { ATOI_OK, ATOI_BADCHAR };

//
// Perform an atoi on str storing result in dest,
// returning whether the conversion was successful (ATOI_OK) or
// an error occurred (ATOI_BADCHAR).
// No radix support.
// atoi_check(str :: pointer<char>(), dest :: pointer<int>()) -> ATOI_OK | ATOI_BADCHAR;
static int atoi_check (char *str, int *dest) {
	int v, sign, valid;

	v = 0;
	sign = 1;
	if(*str == '-') {
		sign = -1;
		++str;
	}
	valid = 0;
	while (*str >= '0' && *str <= '9') {
		v = v * 10 + (*str - '0');
		++str;
		valid = 1; // at least one valid character
	}
	if (valid)     // peek back a character and check if it's in range
		--str;
	if (*str >= '0' && *str <= '9') {
		*dest = v * sign;
		return ATOI_OK;
	}

	return ATOI_BADCHAR;
}

///
/// Random
///

// TODO: implement in c4m, also srand() etc
int random () {
	int fd, result, i;
	if ((fd = open("/dev/urandom", 0)) < 0) {
		if ((fd = open("/dev/random", 0)) < 0) {
			printf("u0: random() failure - cannot read from /dev/urandom or /dev/random\n");
			return 0;
		}
	}
	if ((i = read(fd, &result, sizeof(int)) <= 0)) {
		printf("u0: random() failure - read returned %d\n", i);
	}
	close(fd);
	return result;
}

///
/// Process functions
///

// TODO: remove these check once preprocessor is in
#ifndef __c4_info
int __c4_info () { return __c4_opcode(OP_C4INFO); }
#endif
#ifndef __time
// Don't bother trapping this, too slow
//int __time () { return __c4_opcode(OP_TIME); }
#endif
#ifndef __c4_cycles
int __c4_cycles () { return __c4_opcode(OP_TASK_CYCLES); }
#endif
void halt () { __c4_opcode(OP_HALT); }

///
/// Platform functions
///

// Is the current platform plain C4, with no support for CTRL+C?
int platform_is_c4_plain () {
	return __c4_info() & C4I_C4;
}

///
/// isXYZ functions
///

int isnum (char c) { return (c >= '0' && c <= '9'); }

///
/// Initialization
///
static int __attribute__((constructor)) __u0_init (int *c4r) {
	int r;

	__u0_c4r = c4r;

	if ((r = __u0_ops_init())) return r;
	if ((r = __u0_atexit_init())) return r;

	// Install default signal handlers
    if (U0_DEBUG) printf("u0: signal setup start\n");
	signal(SIGHUP,  (int *)&default_sighup);
	signal(SIGINT,  (int *)&default_sigint);
	signal(SIGQUIT, (int *)&default_sigquit);
	signal(SIGILL,  (int *)&default_sigill);
	signal(SIGTRAP, (int *)&default_sigtrap);
	signal(SIGABRT, (int *)&default_sigabrt);
	signal(SIGTERM, (int *)&default_sigterm);
	signal(SIGKILL, (int *)&default_sigkill);
    if (U0_DEBUG) printf("u0: signal setup done\n");
    // Cache pid and parent
    __u0_pid = __c4_opcode(OP_USER_PID);
	__u0_parent = __c4_opcode(OP_USER_PARENT);
    if (U0_DEBUG) printf("u0: cached pid %d parent %d\n", __u0_pid, __u0_parent);
	return 0;
}

#endif

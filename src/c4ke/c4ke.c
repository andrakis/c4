///
// C4 Kernel Experiment (C4KE)
//
// A multitasking kernel with a primitive emergency shell.
//
// Invocation: make run                                   # Simple invocation (runs under c4m)
//             make run-alt                               # Run with -a flag to c4m, fixes a timing issue
//             make run-c4                                # Run under c4 (slow)
//             make c4rs && ./c4m load-c4r.c -- c4ke.c4r  # Use compiled .c4r file
//             ./c4 c4m.c load-c4r.c c4ke.c               # Run from c4 (slow) without loader
//             ./c4 c4m.c load-c4r.c -- c4ke              # Run from c4 (slow) loader and compiled .c4r file
//             ./c4m load-c4r.c c4ke.c                    # Run from c4m without using loader
//             ./c4m load-c4r.c -- c4ke                   # Run from c4m using loader and compiled .c4r file
//
// You can specificy a number of different command-line options. Try --help for a listing.
//
// The kernel can be run inside itself:
//
//             eshell>
//             toggle psf                                 # Enable ps listing while running below commands
//             c4ke                                       # Run as a normal process (takes over from running kernel)
//             c4m load-c4r.c -- c4ke                     # Run under a c4m interpreter
//             c4 c4m.c load-c4r.c -- c4ke                # Run under a c4m interpreter within c4
//
// This kernel has some optional extras if compiled with them:
//  - c4ke_ipc.c  - InterProcess Communication
//  - c4ke_plus.c - Support for advanced features of c4plus
//
// See u0.c for the user mode interface to the kernel. u0.c is used for most programs compiled to
// run under C4KE.
//
// This kernel focuses on being as simple as possible. Features such as filesystems are implemented
// in microservices (see c4ke.vfs.c) rather than being part of the kernel. There are two main reasons:
//  - Keep the kernel simple and small.
//  - Keep it compatible with c4m. C4CC supports many more features than c4m, and services can take
//    advantage of these improvements.
//
// The kernel supports (TODO but has not implemented) message passing and waiting (with timeouts.)
//
// This program compiles but does nothing useful under GCC.
// This GCC-compatibility is provided so that IDE's can syntax check the file.
// The resulting file can be run, but cannot task schedule so the kernel immediately shuts down.
//
// Task switching is accomplished in two ways:
//   * Co-operative: schedule(), using a custom opcode that invokes a
//     trap handler, which saves registers and returnpc to the task, loads up the
//     new values from the target task, and "returns" to the newly loaded task.
//   * A cycle-based interrupt that calls an interrupt handler, which switches task automatically.
//
// Tasks contain, among many other things, the register values needed to return to
// the task later. There is no virtual machine or other abstraction, each task is
// C4 code running in C4m. By having access to the registers during a trap, we can
// change what code is running in C4m.
//
// This means that tasks cannot exit by themselves, as their stack is still
// being used at the point of call to exit() or returning from main.
// Instead, the idle task (task_idle) cleans up tasks that have finished, as
// their stack is no longer active.
//
// Programs are compiled to .c4r files, the "C4 Relocatable" executable.
// These executables are not interpreted, merely loaded and called directly
// from the code that loaded it, as any other C function would be.
//
// Note that C4 only supports one keyboard entry method - a very primitive
// readline supported by libc that allows text to be entered. Arrow keys
// and other navigation are not supported, and the entire kernel is stopped
// until the user presses enter, which disables task switching while awaiting
// input. It does not seem feasible to implement any other method without
// significantly altering C4 itself.
//
// The shell can run programs in the background using the & symbol, just like
// linux. Otherwise, a program will run in the foreground until it is done.
// When backgrounding a task in this way, control will switch back to the shell
// and as above this will pause the entire kernel. Hitting enter will call
// schedule() and allow other tasks to run, until they call schedule() or are
// pre-emptively interrupted by the cycle interrupt.
//
//
// Job control:
//  - The command 'fg' returns focus to the last started command, if possible.
//    TODO: You cannot currently use 'fg x' to foreground job x.
//  - The command 'kill x' sends a SIGTERM signal to the given process id.
//    TODO: Not implemented.
//
//
// Shell notes:
// Any .c4r file can be run, and you do not need to specify the .c4r part of the
// file, it will be appended automatically.
// Example:
//   c4ke eshell>
//   c4 -s c4m.c &
//
// Note that buffers cannot be flushed, so the shell must present the input
// prompt on a new line.
//
// The kernel shuts down once the init process exits.
//
// TODO:
// - Implement stack trace
//   -> can simulate LEV then check if TLEV, simulate it if necessary
//   -> can use tasks c4r structure, then fall back to loaded modules (TODO) or
//      using the kernels (TODO)
// Not doing:
// - Load balance: use a percentage of the cycle interrupt interval based on number
//   of used process slots.
// - Find cause of random segfault. Sometimes the kernel will crash, especially
//   under high load (eg, 100 processes running.)
//   - is c4m trap overwriting currently pushed arguments? it must be, as top is causing a crash
//     when interrupted by the cycle interrupt.
//   - TRAP_SEGV and TRAP_OPV were added to try to narrow down the cause of the crash.
//     -> c4m has specific checks that slow down execution to provide these additional traps.
//     -> Sometimes the segfault happens outside of these checks and the kernel crashes.
//   - OpenRISC 1000: segfaulting due to bad argc value (several million!)
//   - The best the kernel can do right now is terminate the faulting process, but
//     stack traces would help narrow down where the issue is.
// - Implement relative functions as a kernel module provided set of extended
//   opcodes. (Undecided, would dramatically reduce performance.)
//   - Update c4rlink to change all patches into relative instructions
//
// 2024 / 10 / 04 - Implemented kernel extensions.
// 2024 / 09 / 09 - Fixed kernel idle sleep time, uses correct value. Also fixed
//                  c4m to actually use usleep().
// 2024 / 07 / 07 - Removed ethereal mode, kernel now waits on init process pid.
// 2024 / 07 / 06 - Reworked kernel to use init process, shutdown once complete.
// 2024 / 07 / 05 - Added support for waiting on a pid.
// 2024 / 07 / 04 - Reformat .c4r to place symbols at very end; don't load symbols
//                  by default.
// 2024 / 04 / 01 - Added signal handling.
// 2023 / 11 / 20 - Added critical path functions to avoid interrupts when
//                  updating kernel structures.
// 2023 / 11 / 19 - Added sleep, STATE_WAITING for sleep.
// 2023 / 11 / 18 - Added timekeeping via /proc/uptime or proper time calls.
// 2023 / 11 / 12 - Added cycle timekeeping.

#include "c4.h"
#include "c4m.h"
#define NO_LOADC4R_MAIN
#include "load-c4r.c"

///
// Kernel configuration
///
/// Feel free to play around with the values in this enum
enum {                         // Main configuration section
	KERN_TASK_COUNT = 128,     // Fixed count until updated to use linked list
	TASK_STACK_SIZE = 0x1000,  // How much stack memory to allocate to tasks.
	SIGNAL_MAX = 64,           // How many signals are supported
	// Minimum acceptable cycles between cycle-based interrupt.
	// Values below this may crash the kernel.
	// 900 seems to crash fairly consistently, 901 only sometimes.
	KERNEL_CYCLES_MIN = 1000,
	// Maximum pre-emptive interrupts per second to aim for.
	IH_MAX_CYCLES_PER_SECOND = 10,
	KERNEL_EXTENSIONS_MAX = 32,
	// Give tasks 10 seconds to comply with SIGTERM during shutdown
	KERNEL_FINISH_WAIT_TIME = 10000,
};

///
// Kernel definitions
///
/// These shouldn't need to be changed.

/// Kernel verbosity
enum {
	VERB_NONE = 0,             // Be as quiet as possible
	VERB_MIN  = 10,            // Minimal information
	VERB_MED  = 50,            // Medial information
	VERB_MAX  = 100,           // Maximal information
	VERB_DEFAULT = 50
};

// These settings control how long we measure performance for during boot.
enum { KERNEL_MEASURE_QUICK = 200, KERNEL_MEASURE_SLOW = 1000 };
enum { KERNEL_TFACTOR_QUICK =   5, KERNEL_TFACTOR_SLOW =    1 };
enum { KERNEL_IDLE_SLEEP_TIME = 10000 }; // used with usleep, so in microseconds

// Details for managing the opcode to function vector
enum { CO_BASE = 128, CO_MAX = 128 };

// start_errno values
enum {
	START_NONE,      // No error, success
	START_NOFREE,    // No free task slot
	START_NOSTACK,   // Unable to allocate stack
	START_NOSIG,     // Unable to allocate signal handlers
	START_ARGV,      // Failed to allocate argv
};

// At some stage this will be a symbol available under c4m
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

// Configure codes, for use with CSYS/__c4_configure
enum { C4KE_CONF_CYCLE_INTERRUPT_INTERVAL, C4KE_CONF_CYCLE_INTERRUPT_HANDLER };

static char *VERSION() { return "0.66"; }

// Task state, keep up to date in u0.c
enum {
	STATE_UNLOADED = 0x0,
	STATE_LOADED   = 0x1,
	STATE_RUNNING  = 0x2,
	STATE_WAITING  = 0x4,
	STATE_TRAPPED  = 0x8,  // TODO: Unused
	STATE_ZOMBIE   = 0x20, // Waiting to die
	STATE_NOTRUN   = 0x30
};

// Task privileges
enum {
	PRIV_NONE,        // TODO: Useful?
	PRIV_USER,        // Usermode acess
	PRIV_KERNEL       // Kernel mode access
};

// Wait states
enum {
	WSTATE_TIME,     // Waiting for a time target. WAITARG is target timestamp
	WSTATE_PID,      // Waiting for a process to terminate. WAITARG is the pid.
	WSTATE_MESSAGE,  // Waiting for a message. WAITARG is the "give up" timestamp
};

// Task structure
enum {
	// Since it's the first element, state can be checked with one
	// less array lookup if it is at the start of the array.
	TASK_STATE,       // int, see STATE_
	TASK_ID,          // int, task id
	TASK_NICE,        // int, priority counter
	TASK_NICE_BASE,   // int, priority base level
	TASK_PARENT,      // int, parent task id
	TASK_WAITSTATE,   // int, see WSTATE_
	TASK_WAITARG,     // int, depends on WSTATE_
	TASK_BASE,        // int *, Task BP/SP base address
	TASK_CODE,        // int *, Code address, not used by builtin tasks
	TASK_DATA,        // char *, Data address, not used by builtin tasks
	TASK_REG_A,       // int, saved register A value
	TASK_REG_BP,      // int *, saved register BP value
	TASK_REG_SP,      // int *, saved register SP value
	TASK_REG_PC,      // int *, saved register PC value
	TASK_ENTRY,       // int *, initial PC register value
	TASK_PRIVS,       // int, see PRIV_
	TASK_NAME,        // char *, copied from source and freed at clean
	TASK_NAMELEN,     // int, length of name
	TASK_EXIT_CODE,   // int, exit code for task
	TASK_ARGC,        // int, arg count
	TASK_ARGV,        // char **, pointer to allocated argv
	TASK_ARGV_DATA,   // char **, TASK_ARGV points to here
	TASK_CYCLES,      // int, how many cycles task has run
	TASK_TIMEMS,      // int, time running in milliseconds
	TASK_TRAPS,       // int, how many traps the task has caused
	TASK_C4R,         // int *, ptr to C4R structure
	TASK_SIGHANDLERS, // int *, ptr to SIGH_ structure
	TASK_SIGPENDING,  // int, number of pending signals total
	TASK_MBOX,        // int *, see MBOX_
	TASK_MBOX_SZ,     // int,
	TASK_MBOX_COUNT,  // int,
	TASK_EXCLUSIVE,   // int,
	TASK_EXTDATA,     // int *, task extension data
	TASK__Sz          // task structure size
};

// Kernel task information, the superstructure for kernel task information
// returned via kern_tasks_export() in u0.c
enum {
	KTI_COUNT,
	KTI_USED,
	KTI_LIST,  // see KTE_*
	KTI__Sz
};

// Kernel task element - keep up to date with u0.c
enum {
	// First element, so can be referenced as *kte instead of kte[KTE_TASK_STATE]
	KTE_TASK_STATE,  // int
	KTE_TASK_ID,     // int
	KTE_TASK_PARENT, // int
	KTE_TASK_NAME,   // char *
	KTE_TASK_NAMELEN,// int
	KTE_TASK_PRIORITY, // int
	KTE_TASK_PRIVS,  // int, see PRIV_*
	KTE_TASK_NICE,     // int
	KTE_TASK_CYCLES, // int
	KTE_TASK_TIMEMS, // int
	KTE_TASK_TRAPS,  // int
	KTE__Sz
};

// Custom opcodes
// TODO: Move to a syscall interface instead of string based opcode lookup.
//       All of these are checked via memcmp in request_symbol()
// TODO: These could be global ints instead, and offset when detecting we're running
//       within C4KE already. That way multiple kernels could run at once. At the moment,
//       each child kernel disables the parent kernel.
enum {
	// Request a symbols opcode
	// (char *symbol) -> integer value of requested symbol
	OP_REQUEST_SYMBOL = 128,
	// Request system flags
	// () -> get system flags
	OP_C4INFO,
	// Switch task manually
	// () -> success (1) or no task to switch to (0)
	OP_SCHEDULE,
	// Wait for a message or timeout
	// (int timeout) -> 0 TODO: only timeout is implemented, messages not
	OP_AWAIT_MESSAGE,
	// Wait for a process to exit
	// (int pid)     -> pid exit code
	OP_AWAIT_PID,
	// Finish a task - not used by user code, put directly onto the stack
	// of a task so that returning from main calls it to cleanly finish.
	OP_TASK_FINISH,
	// Exit a task - similar to OP_TASK_FINISH, but called by the user.
	// (int exit_code) -> does not return
	OP_TASK_EXIT,
	// Indicate which task is currently focused
	// (int pid)
	OP_TASK_FOCUS,
	// Get the number of cycles the task has run
	OP_TASK_CYCLES,
	// TODO: Not implemented, currently the only way to shutdown is for
	//       the init process to exit.
	// Request kernel shutdown
	OP_SHUTDOWN,
	// Request a halt.
	// TODO: does nothing.
	OP_HALT,
	// Request a timestamp, return value in milliseconds.
	// TODO: this is much slower than just using __time(), not really useful
	OP_TIME,
	// Test opcode that returns the value of the BP register
	OP_PEEK_BP,
	// Test opcode that returns the value of the SP register
	OP_PEEK_SP,
	// TODO: not used at all
	//OP_KERN_TASK_START_BUILTIN,
	// TODO: replaced by u0 functions that work better
	//OP_KERN_PRINT_TASK_STATE,
	//OP_KERN_GETLEN_TASK_STATE,
	// Get the current task ID
	OP_KERN_TASK_CURRENT_ID,
	// Check if a task is running
	// (int task_id) -> 1 if running, 0 if not
	OP_KERN_TASK_RUNNING,
	// Request number of allocated tasks
	OP_KERN_TASK_COUNT,
	// Request maximum number of tasks
	OP_KERN_TASKS_MAX,
	// Request a task listing. Returns a KTI_* struct.
	// () -> int *
	OP_KERN_TASKS_EXPORT,
	// Update a task listing with the latest data from the kernel.
	// (int *kti) -> void
	OP_KERN_TASKS_EXPORT_UPDATE,
	// Release a task listing and all data
	OP_KERN_TASKS_EXPORT_FREE,
	// Get the number of running tasks
    OP_KERN_TASKS_RUNNING,
	// Debug command: print a stacktrace of the current program
	OP_DEBUG_PRINTSTACK,
	// Debug command: print a debug kernel state
	OP_DEBUG_KERNELSTATE,
	// Start a .c4r file. Uses task_loadc4r.
	// int __user_start_c4r(int argc, char **argv);
	// TODO: Move to a system request interface. This is temporary.
	//       Probably not very secure, since kernel will copy the given argv
	//       based on argc.
	OP_USER_START_C4R,
	// Sleep the specified number of milliseconds
	// int sleep(int ms)
	OP_USER_SLEEP,
	// Get the process id of the current task
	OP_USER_PID,
	// Get the parent process id of the current task
	OP_USER_PARENT,
	// Setup a signal handler
	OP_USER_SIGNAL,
	// Send a signal to a process
	OP_USER_KILL,
	// Update the name of the current task. Badly named?
	OP_CURRENTTASK_UPDATE_NAME,
	// Debugging functions to disable/enable the cycle based interrupt
	OP_KERN_REQUEST_EXCLUSIVE,
	OP_KERN_RELEASE_EXCLUSIVE,
	OP_EXTENSIONS_START,       // Not used directly, kernel extensions will start from this
	                           // number when registering opcodes.
};

static int request_symbol (char *symbol) {
	if (!memcmp(symbol, "OP_HALT", 7)) return OP_HALT;
	if (!memcmp(symbol, "OP_TIME", 7)) return OP_TIME;
	if (!memcmp(symbol, "OP_C4INFO", 9)) return OP_C4INFO;
	if (!memcmp(symbol, "OP_PEEK_BP", 10)) return OP_PEEK_BP;
	if (!memcmp(symbol, "OP_PEEK_SP", 10)) return OP_PEEK_SP;
	if (!memcmp(symbol, "OP_SCHEDULE", 11)) return OP_SCHEDULE;
	if (!memcmp(symbol, "OP_USER_PID", 11)) return OP_USER_PID;
	if (!memcmp(symbol, "OP_USER_KILL", 12)) return OP_USER_KILL;
	if (!memcmp(symbol, "OP_AWAIT_PID", 12)) return OP_AWAIT_PID;
	if (!memcmp(symbol, "OP_TASK_EXIT", 12)) return OP_TASK_EXIT;
	if (!memcmp(symbol, "OP_TASK_FOCUS", 13)) return OP_TASK_FOCUS;
	if (!memcmp(symbol, "OP_USER_SLEEP", 13)) return OP_USER_SLEEP;
	if (!memcmp(symbol, "OP_TASK_CYCLES", 14)) return OP_TASK_CYCLES;
	if (!memcmp(symbol, "OP_USER_SIGNAL", 14)) return OP_USER_SIGNAL;
	if (!memcmp(symbol, "OP_TASK_FINISH", 14)) return OP_TASK_FINISH;
	if (!memcmp(symbol, "OP_USER_PARENT", 14)) return OP_USER_PARENT;
	if (!memcmp(symbol, "OP_AWAIT_MESSAGE", 16)) return OP_AWAIT_MESSAGE;
	if (!memcmp(symbol, "OP_REQUEST_SYMBOL", 17)) return OP_REQUEST_SYMBOL;
	if (!memcmp(symbol, "OP_USER_START_C4R", 17)) return OP_USER_START_C4R;
	if (!memcmp(symbol, "OP_KERN_TASKS_MAX", 17)) return OP_KERN_TASKS_MAX;
	if (!memcmp(symbol, "OP_KERN_TASK_COUNT", 18)) return OP_KERN_TASK_COUNT;
	if (!memcmp(symbol, "OP_DEBUG_PRINTSTACK", 19)) return OP_DEBUG_PRINTSTACK;
	if (!memcmp(symbol, "OP_DEBUG_KERNELSTATE", 20)) return OP_DEBUG_KERNELSTATE;
	if (!memcmp(symbol, "OP_KERN_TASK_RUNNING", 20)) return OP_KERN_TASK_RUNNING;
	if (!memcmp(symbol, "OP_KERN_TASKS_RUNNING", 21)) return OP_KERN_TASKS_RUNNING;
	if (!memcmp(symbol, "OP_KERN_TASK_CURRENT_ID", 23)) return OP_KERN_TASK_CURRENT_ID;
	//if (!memcmp(symbol, "OP_KERN_PRINT_TASK_STATE", 24)) return OP_KERN_PRINT_TASK_STATE;
	if (!memcmp(symbol, "OP_KERN_REQUEST_EXCLUSIVE", 25)) return OP_KERN_REQUEST_EXCLUSIVE;
	if (!memcmp(symbol, "OP_KERN_RELEASE_EXCLUSIVE", 25)) return OP_KERN_RELEASE_EXCLUSIVE;
	//if (!memcmp(symbol, "OP_KERN_GETLEN_TASK_STATE", 25)) return OP_KERN_GETLEN_TASK_STATE;
	if (!memcmp(symbol, "OP_KERN_TASKS_EXPORT_FREE", 25)) return OP_KERN_TASKS_EXPORT_FREE;
	if (!memcmp(symbol, "OP_KERN_TASKS_EXPORT_UPDATE", 27)) return OP_KERN_TASKS_EXPORT_UPDATE;
	if (!memcmp(symbol, "OP_KERN_TASKS_EXPORT", 20)) return OP_KERN_TASKS_EXPORT;
	//if (!memcmp(symbol, "OP_KERN_TASK_START_BUILTIN", 26)) return OP_KERN_TASK_START_BUILTIN;
	if (!memcmp(symbol, "OP_CURRENTTASK_UPDATE_NAME", 26)) return OP_CURRENTTASK_UPDATE_NAME;

	printf("c4ke: symbol '%s' not found\n", symbol);

	return 0;
}

// C4INFO state - must match that in c4m.c
enum {
	C4I_NONE = 0x0,  // No C4 info
	C4I_C4   = 0x1,  // Ultimately running under C4
	C4I_C4M  = 0x2,  // Running under c4m (directly or C4)
	C4I_C4P  = 0x4,  // Running under c4plus
	C4I_HRT  = 0x10, // High resolution timer
	C4I_SIG  = 0x20, // Signals supported
	C4I_C4KE = 0x40, // C4KE is running
};
static int c4_info;
static void print_c4_info () {
	if (c4_info == C4I_NONE) printf("(unknown)");
	if (c4_info & C4I_C4) printf("C4+");
	if (c4_info & C4I_C4M) printf("C4M");
	if (c4_info & C4I_C4P) printf("C4Plus");
	if (c4_info & C4I_HRT) printf("+HighResTimer");
	if (c4_info & C4I_SIG) printf("+Signals");
	if (c4_info & C4I_C4KE) printf(" under C4KE");
	printf(" %dbit", sizeof(int) * 8);
}


///
/// Signals
///
// Signal handlers structure
enum {
	SIGH_PENDING,      //
	SIGH_BLOCKED,
	SIGH_ADDRESS,      // C4 code address of handler
	SIGH__Sz
};

//enum {
//    SIGHUP = 1, SIGINT, SIGQUIT, SIGILL, SIGTRAP, SIGABRT,
//    // Default caught but can be overridden
//    SIGTERM,
//    // Last chance for task to quit nicely
//    SIGKILL,
//	SIGSEGV = 11
//};

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
  SIGSYS      ,
  SIGRTMIN = 32, // ...
  SIGRTMAX = 63, SIGMAX = 64 // Should match SIGNAL_MAX
};

// Kernel extensions
enum {
	KEXT_STATE,        // int, See KXS_*
	KEXT_NAME,         // char *
	KEXT_INIT,         // int *, ptr to init function if any
	KEXT_START,        // int *, ptr to start (called just before tasks execute) if any
	KEXT_SHUTDOWN,     // int *, ptr to shutdown function if any
	KEXT__Sz
};

// KEXT_STATE values
enum {
	KXS_NONE,             // Not in use
	KXS_REGISTERED = 0x1, // Present and registered
	KXS_ERROR = 0x2       // Error during init
};

// Kernel extension callback function return values
enum {
	KXERR_NONE = 0,    // No error
	KXERR_FAIL = 1,    // Error of some sort (see kernel_ext_errno)
};

///
// Kernel main data
///
static int *kernel_tasks;            // struct TASK*
static int *kernel_task_current;     // struct TASK*
static int *kernel_task_idle;        // struct TASK*
static int  kernel_task_id_counter;
static int  kernel_max_slot;
static int  kernel_task_extdata_size;// For TASK_EXTDATA member of TASK
static int *kernel_task_focus; // Interactive focus, hacky
static int  kernel_verbosity;
static int  kernel_loadc4r_mode; // defaults to not loading symbols unless -g is given
static int  kernel_shutdown;
static char*kernel_init,
           *kernel_default_init;
static int  kernel_init_argc,
           *kernel_init_task;
static char **kernel_init_argv;
static int  kernel_running;
static int *kernel_c4r;
static int  kernel_start_time;
static int  kernel_schedule_time;
static int  kernel_shutdown_time;
static int *kernel_extensions, kernel_ext_count, kernel_ext_errno;
static int  kernel_ext_initialized;
// Testing modes
static int enable_measurement; // perform speed measurement at startup
static int enable_test_tasks;  // launch internal test tasks
static int kernel_cycles_count, kernel_cycles_base;
static int kernel_cycles_force;

// Track task counts
static int  kernel_tasks_unloaded, kernel_tasks_loaded;
static int  kernel_tasks_running, kernel_tasks_waiting;
static int  kernel_tasks_trapped;
static int  kernel_tasks_zombie;
static int  kernel_ips;        // Rough measure of instructions per second
// Preserve old cycle interrupt details in these variables.
static int old_ih_cycle_interval, *old_ih_cycle_handler;
// And signal handlers
static int *old_sig_int;//, *old_sig_segv;
static int *custom_opcodes;
static int start_errno; // see START_*
static int kernel_task_find_iterator;
// TODO: removed
//static int kernel_hlt_count; // Tracks tasks call to halt, except for the idle task
static int last_trap_handler;
// Set in main()
static char *readable_int_table;
static int   readable_int_max;
static int kernel_last_cycle;
static int kernel_last_time;
// Initialized to the C4m opcode for TLEV in main(), used only in process_trap.
static int opcode_TLEV;
static int opcode_PSH;
// TODO: no longer used
static int  critical_path_value;

///
// Utility functions
///

static int _strlen (char *s) { char *t; t = s; while(*t) ++t; return t - s; }
static int _strcmp (char *s1, char *s2) { while(*s1 && (*s1 == *s2)) { ++s1; ++s2; } return *s1 - *s2; }
static int _atoi (char *str, int radix) {
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

// Old implementation
// static void print_int_readable (int n) {
// 	int  table_pos;
// 	int  rem;
// 	table_pos = rem = 0;
// 	while(table_pos < readable_int_max && n / 1000 > 0) {
// 		rem = n % 1000;
// 		n = n / 1000;
// 		++table_pos;
// 	}
// 	printf("%ld", n);
// 	if (rem) printf(".%03d", rem);
// 	printf("%c", readable_int_table[table_pos]);
// }
static void print_int_readable (int n) {
	int  table_pos;
	int  x, rem;
	char c;
	table_pos = rem = 0;
	while(table_pos < readable_int_max && (x = n / 1000) > 0) {
		rem = n % 1000;
		n = x;
		++table_pos;
	}
	printf("%ld.%03d %c", n, rem, readable_int_table[table_pos]);
}
static void print_int_readable_using (int n, char *table, int table_max) {
	int  table_pos;
	int  rem;
	char c;
	table_pos = rem = 0;
	while(table_pos < table_max && n / 1000 > 0) {
		rem = n % 1000;
		n = n / 1000;
		++table_pos;
	}
	printf("%ld", n);
	if (rem) printf(".%03d", rem);
	if ((c = table[table_pos]) && c != ' ')
		printf("%c", c);
}
static void print_time_readable (int ms) {
	print_int_readable_using(ms, "m ", 2);
}

// strcpy with allocation
static char *k_strcpy_alloc (char *source) {
	int len, *s;
	char *dest, *t;
	//if (source == 0) return 0; // allow nuls
	if (source == 0) {
		printf("c4ke: k_strcpy_alloc - source is null\n");
		source = "(k_strcpy_alloc)";
	}
	// Find length
	len = 0; t = source;
	while (*t++) ++len;
	// Allocate buffer
	if (!(dest = malloc(len + 1))) {
		printf("c4ke: memory allocation failure in k_strcpy_alloc\n");
		return 0;
	}
	// Copy
	t = dest;
	while (*source)
		*dest++ = *source++;
	*dest++ = 0;
	return t;
}

// Print the readable form of a task state
static void kernel_print_task_state (int s) {
	if(s & STATE_ZOMBIE)       printf("Z");
	else if(s & STATE_WAITING) printf("W");
	else if(s & STATE_TRAPPED) printf("T");
	else if(s & STATE_RUNNING) printf("R");
	else                       printf("U");
}

// Get the string length for the readable form of a task state
static int kernel_getlen_task_state (int s) {
	return 1;
}

// Print task state information
static void kernel_print_task (int *t) {
	printf("c4ke: task '%s' at 0x%lx:\n", t[TASK_NAME], t);
	printf("  Id: %d State: 0x%x ", t[TASK_ID], t[TASK_STATE]);
	kernel_print_task_state(t[TASK_STATE]);
	printf("\n  Registers: A=0x%lx  BP=0x%lx  SP=0x%lx  PC=0x%lx (entry - 0x%lx)\n",
	       t[TASK_REG_A], t[TASK_REG_BP], t[TASK_REG_SP], t[TASK_REG_PC], t[TASK_REG_PC] - t[TASK_ENTRY]);
	//if (t[TASK_STATE] 
	//	printf("\n  Exit code: %d\n", t[TASK_EXIT_CODE]);
}

// TODO: load balancing removed
enum { MAX_BALANCE_RANGE = 2 };
//static void kernel_load_balance () {
//	// Multiply kernel_cycles_base based on number of loaded tasks
//	int new_cycles;
//	// doesn't play well on slow systems
//	return;
//
//	new_cycles = kernel_cycles_base + (kernel_cycles_base * (MAX_BALANCE_RANGE * (100 - ((kernel_tasks_loaded - kernel_tasks_waiting) * 100 / KERN_TASK_COUNT * 100) / 100) / 100));
//	if (new_cycles != kernel_cycles_count) {
//		if (kernel_verbosity >= VERB_MED)
//			printf("c4ke: load balance adjusting cycles to %ld\n", new_cycles);
//		__c4_configure(C4KE_CONF_CYCLE_INTERRUPT_INTERVAL, (kernel_cycles_count = new_cycles));
//	}
//}

// Cycle count helper
static int cycles_difference () {
	int c, d;
	c = __c4_cycles();
	d = c - kernel_last_cycle;
	kernel_last_cycle = c;
	return d;
}

///
// Kernel-callable functions
///
// kernel-callable function to invoke await_pid op
static int await_pid (int pid) {
	return __c4_opcode(pid, OP_AWAIT_PID);
}

static int pid () { return kernel_task_current[TASK_ID]; }


//
// Trap emulator
//

// Kernel function that handles the trap and forwards the result onto the signal handler.
// This is so that signal handlers can be passed some signal information instead of what
// is usually on the stack during a trap handle.
// TODO: unused, signal handlers in user processes get access to the full trap handler stack.
static void process_trap_handler (int *handler, int signal) {
	// TODO
#define handler(x) ((int (*)(int))handler)(x)
	handler(signal);
}
#undef handler
// Takes two extra arguments that are pushed onto the stack.

// Cause a trap on a task.
// NOTE: Cannot be done if the task is currently executing.
// Works the same as c4m's trap function, writing temporary values to the stack
// to be popped by TLEV to return to the previous code position.
//
static void process_trap (int *task, int type, int parameter, int *handler) {
	int *temp, i;
	int *sp, *bp, *pc, a;

	if (kernel_task_current == task) {
		printf("c4ke: process_trap() BUG current task would be modified!\n");
		return;
	}

	// Copy the segfault fix from c4m
	if (!handler) {
		printf("c4ke: process_trap() BUG handler not set!\n");
		printf("c4ke: Trap type %d, offending instruction at 0x%X, sp=0x%X\n", type, pc - 1, sp);
		return;
	}

	sp = (int *)task[TASK_REG_SP];
	bp = (int *)task[TASK_REG_BP];
	pc = (int *)task[TASK_REG_PC];
	a  = task[TASK_REG_A];
	temp = sp;

	// Push details used by TLEV
	// Push instruction and trap number
	*--sp = type;          // printf("*sp(0x%X) = trap type %d\n", sp, *sp);
	*--sp = parameter;     // printf("*sp(0x%X) = parameter %d (at 0x%X)\n", sp, *sp, pc - 1);
	// Push the registers. These can be updated, they are restored by TLEV.
	*--sp = (int)a;        // printf("*sp(0x%X) = a %d\n", sp, a);
	*--sp = (int)bp;       // printf("*sp(0x%X) = bp 0x%X\n", sp, *sp);
	*--sp = (int)temp;     // printf("*sp(0x%X) = sp 0x%X\n", sp, *sp);
	*--sp = (int)pc;       // printf("*sp(0x%X) = returnpc 0x%X\n", sp, *sp);
	*--sp = (int)&opcode_TLEV; // set LEV returnpc to TLEV instruction address
	*--sp;                 // Next position is our handler bp
	// Update BP as if we entered a function, allowing arguments to be referenced.
	// We peek at the ENT x that was skipped to read the number of local variables.
	bp = sp;               // Update handler bp
	*sp = (int)bp;         // Set LEV bp to handler bp
	// Adjust stack to account for local variables
	sp = sp - *(handler + 1); // printf("adjusted stack using ENT %d\n", *(handler - 1));
	// printf("Arguments pushed: %d, sp=0x%X\n", t - sp, sp);
	// Update passed in registers and set pc trap handler after ENT x opcode.
	task[TASK_REG_SP] = (int)sp;
	task[TASK_REG_BP] = (int)bp;
	task[TASK_REG_PC] = (int)(handler + 2); // +2 to skip ENT x
}

//
// Critical path functions
//
// Trap handlers may be interrupted by c4m, at present by the cycle interrupt.
// These function disable the cycle interrupt during critical paths where an
// interrupt could leave the kernel in an inconsistent state.
//
static void critical_path_start () {
	//if (!critical_path_value)
		__c4_configure(C4KE_CONF_CYCLE_INTERRUPT_INTERVAL, 0);
	//++critical_path_value;
	//printf("c4ke: critical path value now at %d\n", critical_path_value);
}
static void critical_path_end() {
	//if (critical_path_value && !--critical_path_value) {
		__c4_configure(C4KE_CONF_CYCLE_INTERRUPT_INTERVAL, kernel_cycles_count);
	//	critical_path_value = 0;
	//}
	//printf("c4ke: critical path value now at %d\n", critical_path_value);
}

//
// Timekeeping helpers
//

static int time_difference () {
	int t, d;
	t = __time();
	d = (t - kernel_last_time);
	kernel_last_time = t;
	return d;
}

// Update the current task's timekeeping details.
static void current_task_timekeeping () {
	kernel_task_current[TASK_CYCLES] = kernel_task_current[TASK_CYCLES] + cycles_difference();
	kernel_task_current[TASK_TIMEMS] = kernel_task_current[TASK_TIMEMS] + time_difference();
	++kernel_task_current[TASK_TRAPS];
}

// Update the kernel tasks' timekeeping details.
static void kernel_task_timekeeping () {
	kernel_tasks[TASK_CYCLES] = kernel_tasks[TASK_CYCLES] + cycles_difference();
	kernel_tasks[TASK_TIMEMS] = kernel_tasks[TASK_TIMEMS] + time_difference();
}

static void trap_enter() {
	critical_path_start();
	current_task_timekeeping();
}

// Called by all traps to restore interrupts and task state
static void trap_exit() {
	// Assign task keeping
	kernel_task_timekeeping();
	critical_path_end();
}

///
// Scheduling and task manipulation
///

// Perform scheduling and switch to another active task.
// @return 1 on successful switch, 0 on no other task to switch to
int schedule () {
	// TODO: trap_exit() here?
	//trap_exit();
	return __c4_opcode(OP_SCHEDULE);
}

// TODO: make me a macro!
static void kernel_task_wake (int *task) {
	// No checking done to see if task is valid
	task[TASK_STATE] = task[TASK_STATE] & ~(STATE_WAITING);
	--kernel_tasks_waiting;
    // TODO: this line causes more problems than its worth
	// task[TASK_WAITSTATE] = task[TASK_WAITARG] = 0;
}

static int kernel_is_task_running (int *task) {
	return task[TASK_STATE] & STATE_RUNNING;
}

// Find a task to run.
// Also handles certain wait states like sleeping.
// This function is probably inefficient
// TODO: re-enable find_mask if appropriate
// TODO: the logic could be much simplified, but attempts to do so keep breaking task switching.
static int *kernel_task_find () {
	int c, i, *t, ts, ws, wa, *result, ms, n, nb;
	//int cycles;
	int *backup_task, backup_task_nice, backup_task_iterator;
	int  tsw;
	i = kernel_task_find_iterator;
	result = backup_task = (int *)(c = 0);
	ms = kernel_max_slot + 1;
	// cycles = __c4_cycles();
	backup_task_nice = 100;
	// backup_task_iterator = 0;

	//while (!result && ++c < KERN_TASK_COUNT) {
	//	i = (i + 1) % KERN_TASK_COUNT;
	while (++c < ms) {
		i = (i + 1) % ms;
		t = kernel_tasks + (TASK__Sz * i);
		//ts = t[TASK_STATE];
		// printf("c4ke: check task 0x%lx, id %ld, state 0x%x\n", t, t[TASK_ID], t[TASK_STATE]);
		// TASK_STATE is the first item in the structure
		if (!(ts = *t) || ts & STATE_ZOMBIE) {
			// Skip empty/zombie tasks
		//} else
		//} else if (t == kernel_task_current) {
			// Skip current task
		//} else if (ts & STATE_ZOMBIE) {
			// Skip zombies
		// } else if (ts & STATE_NOTRUN) {
			// Skip ethereal and zombie tasks
		//} else if (ts) { // & find_mask) {
		//} else if ((tsw = ts & STATE_WAITING)) {
		} else if (ts & STATE_WAITING) {
			//ws = t[TASK_WAITSTATE];
			wa = t[TASK_WAITARG];
			if ((ws = t[TASK_WAITSTATE]) == WSTATE_TIME) {
				// kernel_last_time will have been updated recently enough that we
				// need not call __time() but use the last saved value.
				if (kernel_last_time >= wa)
					result = t;
			}
			else if (ws == WSTATE_MESSAGE) {
				// Control returns to op_await_message
				// TODO: message waiting only supports timeout, never delivers messages.
				//if (t[TASK_MBOX_COUNT] > 0) {
				//	printf("c4ke: have %d messages for pid %d\n", t[TASK_MBOX_COUNT], t[TASK_ID]);
				//	result = t;
				//} else
				if (kernel_last_time >= wa) {
					// printf("c4ke: message timeout for pid %d\n", t[TASK_ID]);
					result = t;
				}
			}
			// WSTATE_PID is not checked here, but in kernel_task_finish
			if (result) {
				kernel_task_find_iterator = i;
				t[TASK_NICE] = t[TASK_NICE_BASE];
				// Wake up
				kernel_task_wake(t);
				//if (c4_info & C4I_C4)
				//	printf("c4ke: woke up task %d after %d cycles (crit path: %d)\n", t[TASK_ID], __c4_cycles() - cycles, critical_path_value);
				return t;
			}
		// } else if (!tsw && n > 0) {
		} else if ((n = t[TASK_NICE]) > 0) {
			// TODO: this section feels wrong.
			// Skip if nice counter not met,
			// but allow if nothing else is found
			if ((t[TASK_NICE] = --n) < backup_task_nice) {
				backup_task = t;
				backup_task_nice = n;
				backup_task_iterator = i;
			}
		} else {
			// Satisfies all criteria
			kernel_task_find_iterator = i;
			t[TASK_NICE] = t[TASK_NICE_BASE];
			// if (c4_info & C4I_C4)
			//	printf("c4ke: activating task %d after %d cycles (crit path: %d)\n", t[TASK_ID], __c4_cycles() - cycles, critical_path_value);
			return t;
		}
	}
	if (backup_task) {
		kernel_task_find_iterator = backup_task_iterator;
		backup_task[TASK_NICE] = backup_task[TASK_NICE_BASE];
		// printf("c4ke: returning backup task in slot %i\n", backup_task_iterator);
		return backup_task;
	}
	kernel_task_find_iterator = i; // ?i;
	//if (c4_info & C4I_C4)
	//	printf("c4ke: failed to find task to activate after %d cycles (crit path: %d)\n", __c4_cycles() - cycles, critical_path_value);
	return 0;
}

// Find a free task slot and return it.
// Assumes already running in a critical path section.
// @return 0 on failure (max tasks running), otherwise task address
static int *kernel_task_find_free () {
	int i, *t, loops_remain;
	i = 0;
	t = kernel_tasks;
	loops_remain = KERN_TASK_COUNT;
	while (loops_remain-- && i < KERN_TASK_COUNT) {
		i = (i + 1) % KERN_TASK_COUNT;
		t = kernel_tasks + (TASK__Sz * i);
		if (t[TASK_STATE] == STATE_UNLOADED) {
			// printf("c4ke: find_free found at 0x%lx\n", t);
			return t;
		}
		//t = t + TASK__Sz;
	}

	return 0;
}

// Find a specific task id and return the task structure.
// Assumes already running in a critical path section.
// @return 0 on failure (not found), otherwise task address
static int *kernel_task_find_pid (int pid) {
	int *t, loops_remain;

	if (pid <= 0)
		return 0;

	t = kernel_tasks;
	loops_remain = KERN_TASK_COUNT;
	while (--loops_remain) {
		if (t[TASK_ID] == pid) {
			return t;
		}
		t = t + TASK__Sz;
	}

	return 0;
}

// Clean up a task. Called by idle and the kernel shutdown routine.
// Assumes already in critical path section.
static void kernel_clean_task (int *t) {
	int *p;
	// Free the data used by this process

	if ((p = (int *)t[TASK_NAME])) {
		if (t != kernel_tasks)
			free((char *)p);
	}
	// printf("c4ke: freeing task stack at 0x%lx\n", t[TASK_BASE]);
	if ((p = (int *)t[TASK_BASE])) free(p);
	if ((p = (int *)t[TASK_CODE])) free(p);
	if ((p = (int *)t[TASK_DATA])) free(p);
	if ((p = (int *)t[TASK_ARGV])) free(p);
	if ((p = (int *)t[TASK_ARGV_DATA])) free((char *)p);
	if ((p = (int *)t[TASK_SIGHANDLERS])) free(p);
	if ((p = (int *)t[TASK_C4R])) c4r_free(p);
	if ((p = (int *)t[TASK_EXTDATA])) free(p);
	// Mark as unused and clear other state.
	memset(t, 0, sizeof(int) * TASK__Sz);
	*t = STATE_UNLOADED;
	//t[TASK_ID] = t[TASK_PARENT] = t[TASK_REG_A]  = t[TASK_REG_BP] =
	//             t[TASK_REG_SP] = t[TASK_REG_PC] = t[TASK_ENTRY]  =
	//             t[TASK_PRIVS]  = t[TASK_CYCLES] = t[TASK_TIMEMS] =
	//             t[TASK_ENTRY]  = t[TASK_EXIT_CODE] = t[TASK_TRAPS] =
	//             t[TASK_NICE]   = t[TASK_NICE_BASE] = t[TASK_CYCLES] =
	//             t[TASK_TIMEMS] = t[TASK_SIGPENDING] =
	//             t[TASK_MBOX]   = t[TASK_MBOX_SZ] = t[TASK_MBOX_COUNT] =
	//             t[TASK_WAITSTATE] = t[TASK_WAITARG] = t[TASK_EXCLUSIVE] =
	//			 t[TASK_NAMELEN] =
	//             0;
}

static void currenttask_update_name (char *newname) {
	char *p;
	critical_path_start();
	if ((p = (char *)kernel_task_current[TASK_NAME]) != 0)
		free(p);
	kernel_task_current[TASK_NAME] = (int)k_strcpy_alloc(newname);
	kernel_task_current[TASK_NAMELEN] = _strlen(newname);
	critical_path_end();
}

// TODO: make a macro
// TODO: add macro support to c4cc
static int *kernel_task_sighandler (int *task, int sig) {
	return ((int *)task[TASK_SIGHANDLERS]) + (sig * SIGH__Sz);
}

// Before switching to a task, check if any signals are pending, and if so
// cause a process trap before we switch to it.
static void kernel_before_switch (int *task) {
	int *sigh, i;

	// TODO: need a SIGWAITING or something to tell if the process is already in a signal handler.
	if (task[TASK_SIGPENDING]) {
		sigh = kernel_task_sighandler(task, 0);
		i = 0;
		// Find lowest signal id
		while (i < SIGNAL_MAX) {
			if (sigh[SIGH_PENDING]) {
				process_trap(task, TRAP_SIGNAL, i, (int *)sigh[SIGH_ADDRESS]);
				// TODO: use process_trap_handler and forward the signal nicer
				//process_trap2(task, TRAP_SIGNAL, i, (int *)&process_trap_handler, sigh(SIGH_ADDRESS), i);
				--sigh[SIGH_PENDING];
				--task[TASK_SIGPENDING];
				// break loop
				i = SIGNAL_MAX;
			}
			++i;
			sigh = sigh + SIGH__Sz;
		}
	}
}

static int internal_task_slot (int *task) {
	return (task - kernel_tasks) / TASK__Sz;
}

// Called when a task finishes, by EXIT or returning from main, or some other way.
// Also handles waking tasks when a task finishes.
static void kernel_task_finish (int *task) {
	int s, *t, i, tid;

	// printf("c5ke: kernel_task_finish(%d), exit code = %d\n", task[TASK_ID], task[TASK_EXIT_CODE]);

	// Update kernel counters
	s = task[TASK_STATE];
	if (s & STATE_LOADED) --kernel_tasks_loaded;
	if (s & STATE_RUNNING) --kernel_tasks_running;
	if (s & STATE_WAITING) --kernel_tasks_waiting;
	if (s & STATE_TRAPPED) --kernel_tasks_trapped;
	s = internal_task_slot(task);
	if (kernel_max_slot == s) {
		--kernel_max_slot;
		// printf("c4ke: max slot-- now %d\n", kernel_max_slot);
	}

	// Check if any process is waiting on this task
	t = kernel_tasks;
	i = 0;
	tid = task[TASK_ID];
	while (i <= kernel_max_slot) {
		if (*t & STATE_WAITING) {
			if (t[TASK_WAITSTATE] == WSTATE_PID) {
				// TODO: negative values to check against parent id
				if (t[TASK_WAITARG] == tid) {
					// Found a task waiting on this task's id specifically

					// Update the WAITARG with tasks exit code.
					t[TASK_WAITARG] = task[TASK_EXIT_CODE];
					// printf("c4ke: woke task.%d with task exit code %d\n", t[TASK_ID], task[TASK_EXIT_CODE]);
					// Wake up the task
					kernel_task_wake(t);
				}
			}
		}
		t = t + TASK__Sz;
		++i;
	}
}

///
// Basic opcodes provided by C4KE.
///
static void op_c4info (int trap, int ins, int a, int *bp, int *sp, int *returnpc) {
	// Add extra info flags
	a = __c4_info() | C4I_C4KE;
	trap_exit();
}

static void op_request_symbol (int trap, int ins, int a, int *bp, int *sp, int *returnpc) {
	char *symbol;
	int i;
	// printf("c4ke: symbol request, stack contents:\n");
	// i = -5; while (i < 5) {
	//	printf("c4ke: sp+%d = 0x%x (%d)\n", i, sp[i], sp[i]);
	//	++i;
	// }
	symbol = (char *)sp[1];
	// printf("Requesting symbol '%s'\n", symbol);
	a = request_symbol(symbol);
	// printf("Symbol value: %d\n", a);
	trap_exit();
}

// Retrieve current BP value
static void op_peek_bp (int trap, int ins, int a, int *bp, int *sp, int *returnpc) {
	a = (int)bp;
	// printf("  (peek bp is 0x%lx)\n", bp);
	trap_exit();
}
// Retrieve current SP value
static void op_peek_sp (int trap, int ins, int a, int *bp, int *sp, int *returnpc) {
	a = (int)sp;
	// printf("  (peek sp is 0x%lx)\n", sp);
	trap_exit();
}

// Handle a schedule request.
// We do this by obtaining a new task, saving the register state to the old task,
// and putting the new task register state in place before returning.
// We could also update memory access permissions if that were a thing.
static void op_schedule (int trap, int ins, int a, int *bp, int *sp, int *returnpc) {
	int *next, *curr;

	if ((next = kernel_task_find())) {
		//printf("c4ke: schedule found a task to run, switch from 0x%lx to 0x%lx\n",
		//       kernel_task_current, next);
		// Save current state
		curr = kernel_task_current;
		curr[TASK_REG_A]  = (int)1; // Found a task to switch to
		curr[TASK_REG_BP] = (int)bp;
		curr[TASK_REG_SP] = (int)sp;
		curr[TASK_REG_PC] = (int)returnpc;
		// Load new state
		kernel_before_switch(next);
		a  =        next[TASK_REG_A];
		bp = (int *)next[TASK_REG_BP];
		sp = (int *)next[TASK_REG_SP];
		returnpc = (int *)next[TASK_REG_PC];
		// Update the task pointer
		kernel_task_current = next;
		//printf("c4ke: registers set to bp=0x%X, sp=0x%X, pc=0x%X\n", bp, sp, returnpc);
	} else {
		// No task switch
		a = 0;
		//printf("c4ke: Schedule: no task found\n");
	}

	trap_exit();
}

// Handle a task finishing. This is added to the running function stack so that
// it is called when its entry/main function exits.
// Cleanup is performed by task_idle, because the stack for this task is still
// active until we switch out of it to a new task.
// If this trap is reached, the task is currently active.
// We need to find another task to switch to.
static void op_task_finish (int trap, int ins, int a, int *bp, int *sp, int *returnpc) {
	int *next, *p;

	kernel_task_finish(kernel_task_current);
	// printf("op_task_finish: finding a new task to move to\n");
	kernel_task_current[TASK_STATE] = STATE_ZOMBIE; // Will be collected later
	++kernel_tasks_zombie;

	// Record the exit code
	kernel_task_current[TASK_EXIT_CODE] = *sp;

	if ((next = kernel_task_find())) {
		// TODO: not used on idle task, any reason to? not yet
		kernel_before_switch(next);
	} else {
		// Wake up the idle task and switch to it
		kernel_task_wake(next = kernel_task_idle);
	}

	// printf("op_task_finish: found task, loading %d\n", next[TASK_ID]);
	// Load new state
	a  =        next[TASK_REG_A];
	bp = (int *)next[TASK_REG_BP];
	sp = (int *)next[TASK_REG_SP];
	returnpc = (int *)next[TASK_REG_PC];
	// Update the task pointer
	kernel_task_current = next;
	// printf("op_task_finish: switched to next task 0x%lx\n", next);
	// printf("op_task_finish: returnpc = 0x%lx\n", returnpc);

	trap_exit();
}

// Used by applications that wish to call exit.
// This code is similar to op_task_finish except that the exit code is in a
// different location on the stack.
static void op_task_exit (int trap, int ins, int a, int *bp, int *sp, int *returnpc) {
	int *next, *p;

	//if (kernel_verbosity > VERB_MED)
	//	printf("c4ke: op_task_exit: exit code=%d\n", sp[1]);
	// Record the exit code
	kernel_task_current[TASK_EXIT_CODE] = sp[1];
	kernel_task_finish(kernel_task_current);
	kernel_task_current[TASK_STATE] = STATE_ZOMBIE; // Will be collected later by idle
	++kernel_tasks_zombie;

	if ((next = kernel_task_find())) {
		kernel_before_switch(next);
	} else {
		// Wake up the idle task and switch to it
		kernel_task_wake(next = kernel_task_idle);
	}

	// Load new state
	a  =        next[TASK_REG_A];
	bp = (int *)next[TASK_REG_BP];
	sp = (int *)next[TASK_REG_SP];
	returnpc = (int *)next[TASK_REG_PC];
	// Update the task pointer
	kernel_task_current = next;
	//printf("op_task_exit: switched to next task 0x%lx\n", next);

	trap_exit();
}

// Indicates to the kernel which task is currently focused.
// Allows for signal handlers to be called
static void op_task_focus (int trap, int ins, int a, int *bp, int *sp, int *returnpc) {
	int *t;

	t = kernel_task_find_pid(sp[1]);
	if (!t) {
		printf("c4ke: pid %d not found\n", sp[1]);
	} else {
		kernel_task_focus = t;
		// printf("c4ke: focus set to pid %d\n", kernel_task_focus[TASK_ID]);
	}
	trap_exit();
}

// Wait on a pid and return its exit code.
// int await_pid(int pid) => pid's exit code
// TODO: implement as waitpid with POSIX style
static void op_await_pid(int trap, int ins, int a, int *bp, int *sp, int *returnpc) {
	int pid;

	pid = sp[1];
	if (kernel_task_find_pid(pid)) {
		*kernel_task_current = *kernel_task_current | STATE_WAITING;
		kernel_task_current[TASK_WAITSTATE] = WSTATE_PID;
		kernel_task_current[TASK_WAITARG] = pid;
		++kernel_tasks_waiting;
		trap_exit();
		schedule();
		// Exit code is placed into TASK_WAITARG by kernel_task_finish
		a = kernel_task_current[TASK_WAITARG];
		// if (kernel_verbosity > VERB_MED)
		//	printf("c4ke: op_await_pid returning, a = %d\n", a);
	} else {
		// If task not running, no point in waiting
		printf("c4ke: await_pid(%d) - process %d not found\n", pid, pid);
		a = -1;
		trap_exit();
	}
}

// int sleep (int ms)
// always returns 0, unlike POSIX which may get interrupted sleep
static void op_user_sleep (int trap, int ins, int a, int *bp, int *sp, int *returnpc) {
	*kernel_task_current = *kernel_task_current | STATE_WAITING;
	kernel_task_current[TASK_WAITSTATE] = WSTATE_TIME;
	++kernel_tasks_waiting;
	// Use kernel_last_time, since __time() was called just prior to this opcode handler
	kernel_task_current[TASK_WAITARG] = kernel_last_time + sp[1];
	// Update kernel times
	trap_exit();
	schedule();
}

///
// Task manipulation
///

// Attempt to start a builtin task using a pointer to a function.
// On a failure (return code 0), start_errno is set with the reason.
//   - See above START_ enum.
// @param entry   The code entry point. The function should have the same syntax
//                as any normal main function. Obtained via (int *)&function
// @param argc    Pushed as arguments
// @param argv    Copied, then pushed as arguments
// @param name    Name of the task
// @param privileges   See PRIV_ enum
// @return 0 on failure (with start_errno set), or on success the task structure
//         created.
static int *start_task_builtin (int *entry, int argc, char **_argv, char *name, int privileges) {
	int    *t, *temp, *sp, *bp, *sigh;
	int    argv_size, i;
	char **argv, *argv_data, *s;
	int    slot;
	int    require_critical;

	if (kernel_running)
		critical_path_start();

    start_errno = START_NONE;
	if (!(t = kernel_task_find_free())) {
		start_errno = START_NOFREE;
		if (kernel_running)
			critical_path_end();
		return 0;
	}
	if (kernel_verbosity >= VERB_MAX)
		printf("c4ke: found free task at 0x%lx\n", t);

	if (!(bp = sp = malloc(TASK_STACK_SIZE))) {
		free(t);
		start_errno = START_NOSTACK;
		if (kernel_running)
			critical_path_end();
		return 0;
	}
	memset(sp, 0, TASK_STACK_SIZE);
	if (kernel_verbosity >= VERB_MAX)
		printf("c4ke: allocated %d bytes for task stack at 0x%lx\n", TASK_STACK_SIZE, bp);

	if (!(sigh = malloc((i = sizeof(int) * SIGH__Sz * SIGNAL_MAX)))) {
		free(t);
		free(bp);
		start_errno = START_NOSIG;
		if (kernel_running)
			critical_path_end();
		return 0;
	}
	memset(sigh, 0, i);
	t[TASK_SIGHANDLERS] = (int)sigh;
	t[TASK_SIGPENDING] = 0;

	start_errno = START_NONE;

	// Make a copy of the argv, otherwise tasks calling this function and returning
	// before the program is started loses the argv values.
	t[TASK_ARGC] = argc;
	if (!(argv = malloc((i = sizeof(char *) * (argc + 1))))) {
		free(t);
		free(bp);
		free(argv);
		printf("c4ke: Unable to allocate argv\n");
		start_errno = START_ARGV;
		if (kernel_running)
			critical_path_end();
		return 0;
	}
	memset(argv, 0, i);
	t[TASK_ARGV] = (int)argv;
	i = argv_size = 0; while(i < argc) argv_size = argv_size + _strlen(_argv[i++]) + 1;
	// Bugfix(1): argv_size of 0 returns null under original c4
	++argv_size;
	if (!(argv_data = malloc(argv_size))) {
		free(t);
		free(bp);
		free(argv);
		start_errno = START_ARGV;
		printf("c4ke: Unable to allocate argv data of %d\n", argv_size);
		if (kernel_running)
			critical_path_end();
		return 0;
	}
	memset(argv_data, 0, argv_size);
	t[TASK_ARGV_DATA] = (int)argv_data;
	if (kernel_verbosity >= VERB_MAX)
		printf("c4ke: Allocated our own argv @ 0x%lx and data @ 0x%lx\n", argv, argv_data);
	// Copy items
	i = 0;
	while(i < argc) {
		argv[i] = argv_data;
		s = _argv[i];
		while (*s) *argv_data++ = *s++;
		*argv_data++ = 0; // nul terminator
		++i;
	}

	// Setup task stack
	t[TASK_BASE] = (int)bp;
	t[TASK_CODE] = 0;
	t[TASK_DATA] = 0;
	bp = sp = (int *)((int)sp + TASK_STACK_SIZE);

	// Avoid a c4 issue where printf can read past the allocated stack
	sp = bp = sp - 6;
	
	// Setup stack so that on return a TASK_FINISH opcode runs.
	*--sp = OP_TASK_FINISH;
	*--sp = opcode_PSH; temp = sp;
	*--sp = argc;
	*--sp = (int)argv;
	*--sp = (int)temp;

	if (kernel_verbosity >= VERB_MAX) {
		printf("c4ke: updating task at 0x%lx\n", t);
	}
	t[TASK_ID] = ++kernel_task_id_counter;
	t[TASK_NICE]   = t[TASK_NICE_BASE] = 10; // default
	// DEBUG renice based on name
	// TODO: add renice interface and don't do it here in the kernel
	if (!_strcmp(name, "kernel/idle")) {
		t[TASK_NICE] = t[TASK_NICE_BASE] = 0;
	} else if (!_strcmp(name, "top")) {
		t[TASK_NICE] = t[TASK_NICE_BASE] = 0;
	}
	t[TASK_PARENT] = kernel_task_current[TASK_ID];
	t[TASK_STATE] = STATE_LOADED | STATE_RUNNING;
	t[TASK_REG_A] = 0;
	t[TASK_REG_BP] = (int)bp;
	t[TASK_REG_SP] = (int)sp;
	t[TASK_REG_PC] = t[TASK_ENTRY] = (int)entry;
	t[TASK_PRIVS]  = privileges;
	t[TASK_C4R]    = 0; // No C4R structure
	t[TASK_NAME]   = (int)k_strcpy_alloc(name);
	if (!t[TASK_NAME]) {
		printf("c4ke: Warning: unable to copy task name\n");
		t[TASK_NAME] = (int)"(copy error)";
	}
	t[TASK_NAMELEN] = _strlen((char *)t[TASK_NAME]);
	t[TASK_EXTDATA] = 0;
	if (kernel_task_extdata_size) {
		if (!(t[TASK_EXTDATA] = (int)malloc(kernel_task_extdata_size))) {
			printf("c4ke: Warning: unable to allocate task extended data of %ld bytes\n", kernel_task_extdata_size);
		}
	}
	--kernel_tasks_unloaded;
	++kernel_tasks_loaded;
	++kernel_tasks_running;
	slot = internal_task_slot(t);
	if (slot > kernel_max_slot) {
		kernel_max_slot = slot;
		// printf("c4ke: max slot++ now %d\n", kernel_max_slot);
	}

	// Trigger a load balance
	// TODO: disabled
	//kernel_load_balance();

	if (kernel_verbosity >= VERB_MAX)
		kernel_print_task(t);

	if (kernel_running)
		critical_path_end();
	return t;
}

//
// Trap functions
//

// Handle a trap before the kernel is ready.
static void early_trap_handler (int trap, int ins, int a, int *bp, int *sp, int *returnpc) {
	int *alt;

	printf("c4ke: early_trap_handler called! printing stack trace:\n");
	alt = kernel_task_current ? (int *)kernel_task_current[TASK_C4R] : 0;
	c4r_print_stacktrace(kernel_c4r, alt, returnpc);
}


// Handle a trap.
//
// This handler allows a number of custom opcodes. It jumps directly into
// the custom handler.
//
// @param trap      The trap signal, see TRAP_*
// @param ins       The instruction code that caused the trap.
// @param a         Register A at time of trap. Can be updated.
//                  Value restored to register A when a TLEV occurs.
// @param bp        Register BP at time of trap. Restored by TLEV.
// @param sp        Register SP at time of trap. Restored by TLEV.
// @param returnpc  Register PC at time of trap. You get the idea.
static void trap_handler (int trap, int ins, int a, int *bp, int *sp, int *returnpc) {
	int *handler;
	//printf("Trap handler: T%d  I%d(0x%X)\n", trap, ins, ins);
	//printf("  SP=0x%X  BP=0x%X  ReturnPC=0x%X\n", sp, bp, returnpc);
	//printf("  opcode handler address: 0x%X\n", (custom_opcodes + (ins - CO_BASE)));
	//printf("  opcode handler value  : 0x%X\n", *(custom_opcodes + (ins - CO_BASE)));

	// Stops timekeeping for task, activates kernel timekeeping
	trap_enter();

	if (trap == TRAP_ILLOP) {
		//handler = (int *)custom_opcodes[ins - CO_BASE];
		handler = (int *)(*(custom_opcodes + (ins - CO_BASE)));

		// TODO: save current privilege and set kernel privilege mode for duration
		//       of custom opcode. Must restore on return.

		// Ensure within range
		//if (ins >= CO_BASE && ins <= CO_BASE + CO_MAX && handler) {
		if (handler) {
			// Adjust stack based on bp for handler we're about to jump into.
			// Reads x from ENT x instruction.
			// Technically we should take 1 off the number since trap_handler
			// already has a local variable, but it's not worth the additional
			// instructions.
			__c4_adjust(*(handler - 1) * -1);
			// Jump to the handler, does not return
			__c4_jmp(handler);
		}

		// Update task info using trap details
		kernel_task_current[TASK_REG_A]  = (int)a;
		kernel_task_current[TASK_REG_BP] = (int)bp;
		kernel_task_current[TASK_REG_SP] = (int)sp;
		kernel_task_current[TASK_REG_PC] = (int)returnpc;
		// Kill the task and schedule()
		printf("c4ke: Custom opcode not found: %d, executed by task %d\n",
			   ins, kernel_task_current[TASK_ID]);
		kernel_print_task(kernel_task_current);
		kernel_task_current[TASK_EXIT_CODE] = -1000;
		kernel_task_finish(kernel_task_current);
		kernel_task_current[TASK_STATE] = STATE_ZOMBIE;
		schedule();
		printf("c4ke: unable to terminate instruction faulting process\n");
		exit(-2);
	}

	// Segfault?
	else if (trap == TRAP_SEGV) {
		// Update task info using trap details
		kernel_task_current[TASK_REG_A]  = (int)a;
		kernel_task_current[TASK_REG_BP] = (int)bp;
		kernel_task_current[TASK_REG_SP] = (int)sp;
		kernel_task_current[TASK_REG_PC] = (int)returnpc;
		printf("c4ke: segfault in process %d! Attempted to read from 0x%lx\n",
		       kernel_task_current[TASK_ID], ins);
		kernel_print_task(kernel_task_current);
		c4r_print_stacktrace(kernel_c4r, (int *)kernel_task_current[TASK_C4R], returnpc);
		// Kill the task and schedule()
		kernel_task_current[TASK_EXIT_CODE] = -1000;
		kernel_task_finish(kernel_task_current);
		kernel_task_current[TASK_STATE] = STATE_ZOMBIE;
		schedule();
		printf("c4ke: unable to terminate segfaulting process\n");
		exit(-4);
	}

	// OPCD fault?
	else if (trap == TRAP_OPV) {
		printf("c4ke: opcd fault in process %d! Attempted to use opcode %d\n",
		       kernel_task_current[TASK_ID], ins);
		kernel_print_task(kernel_task_current);
		c4r_print_stacktrace(kernel_c4r, (int *)kernel_task_current[TASK_C4R], returnpc);
		// Kill the task and schedule()
		kernel_task_current[TASK_EXIT_CODE] = -1000;
		kernel_task_finish(kernel_task_current);
		kernel_task_current[TASK_STATE] = STATE_ZOMBIE;
		schedule();
		printf("c4ke: unable to terminate segfaulting process\n");
		exit(-4);
	}

	// No other traps supported
	else {
		printf("c4ke: Unexpected trap %d\n", trap);
		exit(-1);
	}
}

//
// Custom opcodes
//

// Install a custom opcode handler.
//
// By convention, custom opcodes start at 128.
//
// The handler address recorded skips the ENT x instruction, but the main
// trap handler inspects this instruction to adjust the stack accordingly.
//
// If a custom opcode handler already exists, the program exits. This could
// likely signify an opcode getting trashed because the wrong value is being
// used.
//
// @param opcode    Opcode to install handler for
// @param handler   The handler, obtained using (int *)&function_name
// @return          0 on success, 1 on error (out of range).
static int install_custom_opcode (int opcode, int *handler) {
	int addr;

	if (opcode >= CO_BASE && opcode <= CO_BASE + CO_MAX) {
		addr = opcode - CO_BASE;
		if (custom_opcodes[addr]) {
			// Signal an error since we may be clobbering other custom opcodes
			printf("c4ke: error - attempting to overwrite opcode %d handler\n", opcode);
			exit(-3);
		}
		// Skip the ENT x instruction
		custom_opcodes[addr] = (int)(handler + 2);
		// printf("Custom opcode %d handler at 0x%x installed at 0x%x\n", opcode, handler + 2, &custom_opcodes[addr]);
		return 0;
	}

	return 1;
}

static int schedule_task_mask;


//
// Builtin kernel tasks
//

// A builtin task that attempts to schedule.
// Also cleans up finished tasks, as their stack is not in use when another
// task is running, and is free to cleanup.
static int task_idle (int argc, char **argv) {
	int i, *t;
	int run;
	int spin;
	int zombie_reap_time, zrp;
	int load_balance_time, lbt;

	//printf("idle: task %d starting idle loop\n", kernel_task_current[TASK_ID]);
	run = 1;
	zombie_reap_time = load_balance_time = __time();
	// TODO: c4cc, while(1) should use unconditional jump
	while (1) {
		if (schedule()) {
		} else {
			//printf("idle: would halt now if it was implemented, "
			//       "running tasks: %d (%d loaded)\n",
			//       count_running, count_loaded);
			// kernel_hlt_count = 0;
			// TODO: check for next schedule time
			//i = __time();
			__c4_usleep(KERNEL_IDLE_SLEEP_TIME);
			//printf("(idle: slept for %d usec)\n", __time() - i);
		}

		// a trap has been called recently, use the last timestamps
		zrp = lbt = kernel_last_time;

		// Clear out zombie tasks, and do inventory on task counts
		// Ensure there's actually other tasks running.
		// Disables task switching whilst this is performed.
		// Only occurs after 1 second has passed.
		if (kernel_tasks_zombie) {
			if (zrp - zombie_reap_time >= 1000) {
				zombie_reap_time = zrp;
				i = 0;
				t = kernel_tasks + (TASK__Sz * 2); // Skip kernel and idle tasks
				while (++i < KERN_TASK_COUNT) {
					// Might not see tasks that zombify straight away, but that's ok
					if (t[TASK_STATE] & STATE_ZOMBIE) {
						critical_path_start();
						kernel_clean_task(t);
						--kernel_tasks_zombie;
						++kernel_tasks_unloaded;
						critical_path_end();
					}
					t = t + TASK__Sz;
				}
			}
		}

		// TODO: removed load balancing
		//if (lbt - load_balance_time >= 2000) {
		//	load_balance_time = lbt;
		//	kernel_load_balance();
		//}
	}
}

// A test task that (uselessly) prints counting up to 5
static int task_printloop_1 (int argc, char **argv) {
	int count;
	printf("printloop1.%d: starting loop\n", kernel_task_current[TASK_ID]);
	count = 0;
	while (++count <= 5) {
		printf("printloop1.%d: some output (count %d)\n", kernel_task_current[TASK_ID], count);
		schedule();
	}

	// printf("printloop1.%d: loop done\n", kernel_task_current[TASK_ID]);
	return 0;
}

// A test task that (uselessly) prints counting up to 10
static int task_printloop_2 (int argc, char **argv) {
	int count;
	printf("printloop2.%d: starting loop\n", kernel_task_current[TASK_ID]);
	count = 0;
	while (++count <= 10) {
		printf("printloop2.%d: some output (count %d)\n", kernel_task_current[TASK_ID], count);
		schedule();
	}

	// printf("printloop1.%d: loop done\n", kernel_task_current[TASK_ID]);
	return 0;
}

// A task that loads a .c4r file and runs it by calling the entry point.
// NOTE: Cannot use return in this function, use __c4_opcode(exit_code, OP_TASK_EXIT).
static int task_loadc4r (int argc, char **argv) {
	int i, p, result;
	char *file, *name, *old;
	int *module, *modules;
	char *alt_file;
	char argv_size, *name_ptr, *argv_data, *s;

	file = argv[0];
	p    = kernel_task_current[TASK_ID]; //pid();
#if 0
	//__c4_opcode(OP_DEBUG_PRINTSTACK);
	//printf("task_loadc4r starting...\n");
	//printf("task_loadc4r.%d: Loading '%s'...\n", p, file);
	//printf("task_loadc4r.%d: arguments:\n", p);
	//i = 0; while(i < argc) {
	//printf("task_loadc4r.%d: argv[%d] == '%s'\n", p, i, argv[i]);
	//++i;
	//}
#endif

	// Update task name by concatenating all arguments.
	if (0) {
		// TODO: terribly inefficient, needs to allocate once like the others.
		// TODO: copy the logic from start_task_builtin
		name = strcpycat(argv[0], " ");
		name = c4r_strcpy_alloc(argv[0]);
		i = 1; while (i < argc) {
			old = name;
			name = strcpycat(name, " ");
			// printf("old = 0x%x, new = 0x%x, value = '%s'\n", old, name, name);
			free(old);
			old = name;
			name = strcpycat(name, argv[i]);
			// printf("old = 0x%x, new = 0x%x, value = '%s'\n", old, name, name);
			free(old);
			++i;
		}
	} else {
		// new way
		// count argv length
		i = argv_size = 0; while(i < argc) argv_size = argv_size + _strlen(argv[i++]) + 1;
		++argv_size;
		if (!(name = malloc(argv_size))) {
			printf("c4ke: failed to allocate name correctly\n");
			name = "(out of memory)";
		} else {
			memset(name, 0, argv_size);
			// Copy items
			i = 0;
			name_ptr = name;
			while (i < argc) {
				s = argv[i];
				while (*s) *name_ptr++ = *s++;
				*name_ptr++ = ' '; // space between arguments
				++i;
			}
		}
	}
	currenttask_update_name(name);

	// printf("lc4r: load mode = 0x%x\n", kernel_loadc4r_mode);
	alt_file = 0;
	file = argv[0];
	if (!(module = c4r_load_opt(file, kernel_loadc4r_mode))) {
		// Attempt a version with .c4r appended
		alt_file = strcpycat(file, ".c4r");
		if (!(module = c4r_load_opt(alt_file, kernel_loadc4r_mode))) {
			printf("lc4r: unable to open '%s' or '%s'\n", file, alt_file);
			free(alt_file);
			free(name);
			__c4_opcode(1, OP_TASK_EXIT);
		}
	}
	// printf("lc4r: module %s loaded\n", alt_file ? alt_file : file);

	// c4r_dump_info(module);
	result = -1;
	if (module[C4R_LOADCOMPLETE])
		result = loadc4r_execute(module, argc, argv);
	else
		printf("c4ke: load not complete\n");
	kernel_task_current[TASK_C4R] = (int)module;
	if (alt_file) free(alt_file);

	if (kernel_verbosity >= VERB_MAX)
		printf("task '%s' (%d): exit with status %d\n", name, p, result);
	free(name);
	// opcode exit
	__c4_opcode(result, OP_TASK_EXIT);
}

///
// Interrupt handlers
///

// Cycle interrupt handler.
// This is never directly called via an opcode, it happens as a trap with
// type TRAP_HARD_IRQ.
// Since it isn't a function call, we must take care to save and not modify
// any registers.
static void ih_cycle (int type, int ins, int a, int *bp, int *sp, int *returnpc) {
	// TODO: move to a generic function? duplicates code from schedule
	int *next, *curr;

	//if (critical_path_value) {
	//	printf("c4ke: ih_cycle in critical path!? (level %d)\n", critical_path_value);
	//	return;
	//}

	critical_path_start();
	current_task_timekeeping();

	// Getting too close to stack overflow error, just return.
	// (May lag system)
	//if (sp <= (int *)kernel_task_current[TASK_BASE] + 100) {
	//	printf(" !! c4ke: stack overflow warning, too many interrupts!! \n");
	//	critical_path_end();
	//	return;
	//}

	//printf("c4ke: forcing task schedule\n");
	if ((next = kernel_task_find())) {
		// printf("c4ke: ih_cycle found a task to run, switch from 0x%lx to 0x%lx\n",
		//        kernel_task_current, next);
		// Save current state
		curr = kernel_task_current;
		curr[TASK_REG_A]  = (int)a;
		curr[TASK_REG_BP] = (int)bp;
		curr[TASK_REG_SP] = (int)sp;
		curr[TASK_REG_PC] = (int)returnpc;
		// Load new state
		kernel_before_switch(next);
		a  =        next[TASK_REG_A];
		bp = (int *)next[TASK_REG_BP];
		sp = (int *)next[TASK_REG_SP];
		returnpc = (int *)next[TASK_REG_PC];
		// Update the task pointer
		// printf("c4ke: scheduled next task\n");
		kernel_task_current = next;
	} else {
		// No task switch - probably idle is the only task available
		// printf("c4ke: warning in ih_cycle: no other task found\n");
		// Just return as normal since no other tasks found
	}
	kernel_task_timekeeping();
	critical_path_end();
}

//
// Signal handling
//

// Internal use only, takes a task pointer.
// See kernel_signal for user accessible function
static int internal_signal (int *task, int sig) {
	int *sigh;

	// printf("c4ke: internal_signal(task[%d], %d)\n", task[TASK_ID], sig);

	sigh = kernel_task_sighandler(task, sig);

	if (sigh[SIGH_ADDRESS]) {
		// process_trap(task, TRAP_SIGNAL, sig, (int *)sigh[SIGH_ADDRESS]);
		++task[TASK_SIGPENDING];
		++sigh[SIGH_PENDING];
		// printf("c4ke: Handler: 0x%x  Pending: %d  Blocked: %d\n",
		//	sigh[SIGH_ADDRESS], sigh[SIGH_PENDING], sigh[SIGH_BLOCKED]);
		// TODO: signals wake tasks, even if awaiting on a pid
		kernel_task_wake(task);
	} else {
		printf("c4ke: internal_signal(pid.%d, signal.%d) - no signal handler found\n",
		       task[TASK_ID], sig);
	}
	return 0;

	// Old code
	// printf("c4ke: SIGINT(%d) on task %d, killing it!\n", sig, task[TASK_ID]);
	// task[TASK_STATE] = STATE_ZOMBIE;
	//if (kernel_task_current == task)
	//	schedule();
	//printf("c4ke: Handler: 0x%x  Pending: %d  Blocked: %d\n",
	//	sigh[SIGH_ADDRESS], sigh[SIGH_PENDING], sigh[SIGH_BLOCKED]);
}

// Callable by user functions as kill
static int kernel_signal (int pid, int sig) {
	int *task;
	if (!(task = kernel_task_find_pid(pid)))
		return -1;
	else
		return internal_signal(task, sig);
}

// TODO: moveme
static void dump_tasks () {
	int i, *t;
	i = 0;
	t = kernel_tasks;
	while (i < kernel_max_slot) {
		kernel_print_task(t);
		++i;
		t = t + TASK__Sz;
	}
}

// Callback handler for SIGINT
static void signal_forwarder (int type, int sig, int a, int *bp, int *sp, int *returnpc) {
	// Force override signal to our version of SIGINT
	// TODO: no other signals will work
	//sig = SIGINT;
	if (kernel_shutdown) {
		// Should not happen. Currently does if a signal handler is overwritten by invoking
		// a new c4ke.
		//printf("c4ke: signal_forwarder not removed during kernel shutdown, immediately exiting\n");
		//exit(-100);
		// Tell the task finisher to exit now
		kernel_shutdown = 100;
	}
	trap_enter();
	// printf("c4m: signal_forwarder(%d, %d)\n", type, sig);
	if (kernel_task_focus && (kernel_task_focus[TASK_STATE] & STATE_RUNNING))
		// Signal to the task in focus
		internal_signal(kernel_task_focus, sig);
	else {
		printf("c4ke: Could not forward signal as no task is focused or focused task not running.\n");
		printf("c4ke: As a result, requesting kernel shutdown.\n");
		dump_tasks();
		// Wake kernel task, it begins shutdown
		kernel_task_wake(kernel_tasks);
	}
	trap_exit();
}

//static void sig_segv_handler (int type, int sig, int a, int *bp, int *sp, int *returnpc) {
//	printf("c4ke: SIGSEGV caught on process %d!\n", kernel_task_current[TASK_ID]);
//	kernel_print_task(kernel_task_current);
//	exit(-1);
//}

///
// Kernel extensions
// When added after c4ke.c in the c4cc command line, extensions can access all of
// C4KE's data as well as add in its own opcodes.
///

// Internal
static int kext_initialize () {
	int i;

	if (kernel_ext_initialized)
		return KXERR_NONE;

	// Initialize kernel extension state
	if (!(kernel_extensions = malloc((i = sizeof(int) * KEXT__Sz * KERNEL_EXTENSIONS_MAX)))) {
		printf("c4ke: kext_register failed to allocate %d bytes for kernel extensions\n", i);
		return KXERR_FAIL;
	}
	memset(kernel_extensions, 0, i);
	kernel_ext_count = 0;
	kernel_ext_errno = 0;
	kernel_ext_initialized = 1;

	kernel_task_extdata_size = 0;

	return KXERR_NONE;
}

// Internal
// Stop regular compilers complaining about calling int *'s
#define cb() ((int (*)())cb)()
static int kext_run_all (int callback_type) {
	int i, *kext, *cb, cb_res;

	kext = kernel_extensions;
	i    = 0;
	while (i < KERNEL_EXTENSIONS_MAX) {
		// Save an array lookup by relying on KEXT_STATE being the first element
		if (*kext == KXS_REGISTERED) {
			cb = (int *)kext[callback_type];
			if (cb) {
				cb_res = cb();
				// Special for init
				if (callback_type == KEXT_INIT) {
					if (cb_res != KXERR_NONE) {
						printf("c4ke: kernel extension %s returned error during init, disabled\n",
						       (char *) kext[KEXT_NAME]);
						*kext = KXS_ERROR;
					}
				}
			}
		}
		kext = kext + KEXT__Sz;
		++i;
	}

	return KXERR_NONE;
}
#undef cb

// Register a kernel extension, with optional event callback functions.
// Can be called during a constructor in __attribute__((constructor)) functions.
// - init can be used to add custom opcodes. Called before t
// start can be used to perform code just before the kernel starts executing tasks.
// shutdown is called during kernel shutdown.
static int kext_register (char *name, int *init, int *start, int *shutdown) {
	int *kext;

	// TODO: set kernel_ext_errno
	if (!kernel_ext_initialized) {
		if (kext_initialize() == KXERR_FAIL)
			return KXERR_FAIL;
	}

	if (kernel_ext_count >= KERNEL_EXTENSIONS_MAX) {
		printf("c4ke: kext_register failed because kernel_ext_count(%d) at maximum (%d)\n",
		       kernel_ext_count, KERNEL_EXTENSIONS_MAX);
		return KXERR_FAIL;
	}
	kext = kernel_extensions + (KEXT__Sz * kernel_ext_count++);

	kext[KEXT_STATE] = KXS_REGISTERED;
	kext[KEXT_NAME]  = (int)name;
	kext[KEXT_INIT]  = (int)init;
	kext[KEXT_START] = (int)start;
	kext[KEXT_SHUTDOWN] = (int)shutdown;

	return KXERR_NONE;
}

///
// More opcode handlers using functions above
///


// For now doesn't actually halt
static void op_halt (int trap, int ins, int a, int *bp, int *sp, int *returnpc) {
	// Increment halt counter unless we're idle task
	if (kernel_task_current == kernel_task_idle) {
		// Really halt..somehow
	} else {
		// TODO: removed
		// ++kernel_hlt_count;
	}
	trap_exit();
}

static void op_time (int trap, int ins, int a, int *bp, int *sp, int *returnpc) {
	// The time has just been updated prior to entering this trap, use that
	a = kernel_last_time;
	trap_exit();
}

static void op_task_cycles (int trap, int ins, int a, int *bp, int *sp, int *returnpc) {
	a = kernel_task_current[TASK_CYCLES];
	trap_exit();
}

// Request kernel shutdown: no-op right now, idle enables the kernel task
static void op_shutdown (int trap, int ins, int a, int *bp, int *sp, int *returnpc) {
	if (kernel_task_current == kernel_task_idle ||
	    kernel_task_current == kernel_tasks) {
	} else {
		printf("c4ke: shutdown requested from non-idle task 0x%lx, ignored\n", kernel_task_current);
	}
	trap_exit();
}

static void op_kern_print_task_state (int trap, int ins, int a, int *bp, int *sp, int *returnpc) {
	kernel_print_task_state(sp[1]);
	trap_exit();
}
static void op_kern_getlen_task_state (int trap, int ins, int a, int *bp, int *sp, int *returnpc) {
	a = kernel_getlen_task_state(sp[1]);
	trap_exit();
}
static void op_kern_task_current_id (int trap, int ins, int a, int *bp, int *sp, int *returnpc) {
	a = kernel_task_current[TASK_ID];
	trap_exit();
}
static void op_kern_task_running (int trap, int ins, int a, int *bp, int *sp, int *returnpc) {
	int id, i, *t;
	id = sp[1];
	// printf("c4ke: kern_task_running requested for task %d\n", id);
	// Skip kernel and idle
	t = kernel_tasks + (TASK__Sz * 2);
	i = 2;
	while (i < KERN_TASK_COUNT) {
		if (t[TASK_ID] == id) {
			a = kernel_is_task_running(t);
			trap_exit();
			return;
		}
		++i;
		t = t + TASK__Sz;
	}
	a = 0;
	trap_exit();
}
// Get the current number of tasks
static void op_kern_task_count (int trap, int ins, int a, int *bp, int *sp, int *returnpc) {
	a = kernel_tasks_running;
	trap_exit();
}
// Get the maximum number of tasks
static void op_kern_tasks_max (int trap, int ins, int a, int *bp, int *sp, int *returnpc) {
	a = KERN_TASK_COUNT;
	trap_exit();
}

static void kern_tasks_export_update (int *target) {
	int i, *t, task_name, kti_used, kte_name, kte_size;
	int kms;

	kti_used = target[KTI_USED];
	// Set the used tasks count and cache value for use in while loop
	target[KTI_USED] = (kms = kernel_max_slot + 1);
	// printf("c4ke: kern_tasks_export_update(0x%lx)\n", target);
	// Fill in public details
	target = target + KTI__Sz;
	i = 0;
	t = kernel_tasks;
	kte_size = sizeof(int) * KTE__Sz; // cache for later
	while (++i <= kms) {
		kte_name = target[KTE_TASK_NAME];
		// If a task is present...
		if ((*target = *t)) {
			task_name = t[TASK_NAME];
			// If task name is different or kte_name not set
			// TODO: strcmp that also gathers strlen for less cycles
			// TODO: this whole section is questionable and takes far too many cycles
			//if (kte_name ? _strcmp((char *)task_name, (char *)kte_name) : 1) {
			if (kte_name ? (target[KTE_TASK_NAMELEN] != t[TASK_NAMELEN] && _strcmp((char *)task_name, (char *)kte_name)) : 1) {
				target[KTE_TASK_ID] = t[TASK_ID];
				target[KTE_TASK_PARENT] = t[TASK_PARENT];
				target[KTE_TASK_PRIVS] = t[TASK_PRIVS];
				if (kte_name) free((char *)kte_name);
				if (!(kte_name = target[KTE_TASK_NAME] = (int)k_strcpy_alloc((char *)task_name))) {
					printf("c4ke: KTE alloc failure\n");
					target[KTE_TASK_NAMELEN] = 0;
				} else {
					// Update these values once only
					target[KTE_TASK_NAMELEN] = _strlen((char *)kte_name);
				}
			}
			// Update these values every cycle
			target[KTE_TASK_CYCLES] = t[TASK_CYCLES];
			target[KTE_TASK_TIMEMS] = t[TASK_TIMEMS];
			target[KTE_TASK_TRAPS]  = t[TASK_TRAPS];
			target[KTE_TASK_NICE]   = t[TASK_NICE_BASE];
		} else if (kte_name) {
			// Free KTE and reset data
			free((char *)kte_name);
			memset(target, 0, kte_size);
		}
		t = t + TASK__Sz;
		target = target + KTE__Sz;
	}
	// Clear the rest of the entries
	target = target + KTE__Sz;
	while (++i <= kti_used) {
		if ((kte_name = target[KTE_TASK_NAME])) {
			// Free KTE and reset data
			free((char *)kte_name);
			memset(target, 0, kte_size);
		}
		target = target + KTE__Sz;
		++i;
	}
	// printf("c4ke: kern_tasks_export_update finished\n");
}
static void op_kern_tasks_export (int trap, int ins, int a, int *bp, int *sp, int *returnpc) {
	int sz, *result;
	// printf("c4ke: Kern tasks export triggered by ins @ 0x%lx, task %d\n", returnpc, kernel_task_current[TASK_ID]);
	sz = sizeof(int) * (KTI__Sz + (KERN_TASK_COUNT * KTE__Sz));
	if (!(result = malloc(sz))) {
		printf("c4ke: failed to malloc %d bytes\n", sz);
		a = 0;
		trap_exit();
		return;
	}
	memset(result, 0, sz);

	result[KTI_COUNT] = KERN_TASK_COUNT;
	result[KTI_USED ] = kernel_tasks_loaded;
	result[KTI_LIST ] = (int)(result + KTI__Sz);
	kern_tasks_export_update(result);
	a = (int)result;
	trap_exit();
}
static void op_kern_tasks_export_update (int trap, int ins, int a, int *bp, int *sp, int *returnpc) {
	kern_tasks_export_update((int *)sp[1]);
	trap_exit();
}
static void op_kern_tasks_export_free (int trap, int ins, int a, int *bp, int *sp, int *returnpc) {
	int *kti, *kte, i;

	kti = (int *)sp[1];
	kte = (int *)kti[KTI_LIST];
	i = 0;
	while (i++ < KERN_TASK_COUNT) {
		if (kte[KTE_TASK_NAME]) {
			free((char *)kte[KTE_TASK_NAME]);
			kte[KTE_TASK_NAME] = 0;
		}
		kte = kte + KTE__Sz;
	}

	free(kti);
	trap_exit();
}
static void op_kern_tasks_running (int trap, int ins, int a, int *bp, int *sp, int *returnpc) {
	a = kernel_tasks_running;
	trap_exit();
}

static void op_debug_printstack (int trap, int ins, int a, int *bp, int *sp, int *returnpc) {
	printf("op_debug_printstack:\n");
	printf("  pc: 0x%lx (%ld)\n", returnpc - 1, returnpc - 1);
	printf("  instruction 0x%lx\n", ins);
	printf("  a = 0x%lx (%ld)\n", a, a);
	printf("  bp = 0x%lx\n", bp);
	printf("  sp = 0x%lx\n", sp);
	trap_exit();
}

static void op_debug_kernelstate (int trap, int ins, int a, int *bp, int *sp, int *returnpc) {
	printf("c4ke: kernel state and counters:\n");
	printf("    : tasks_unloaded: %2ld    tasks_loaded: %2ld\n",
	       kernel_tasks_unloaded, kernel_tasks_loaded);
	printf("    : tasks_running:  %2ld    tasks_waiting: %2ld\n",
	       kernel_tasks_running, kernel_tasks_waiting);
	printf("    : tasks_trapped:  %2ld    tasks_zombie:   %2ld\n",
	       kernel_tasks_trapped,  kernel_tasks_zombie);
	printf("    : task iterator:  %2ld    critical path:  %2ld\n",
	       kernel_task_find_iterator, critical_path_value);
	trap_exit();
}

static void op_request_exclusive (int trap, int ins, int a, int *bp, int *sp, int *returnpc) {
	// don't run trap_exit
	a = 0;
	kernel_task_current[TASK_EXCLUSIVE] = 1;
}

static void op_release_exclusive (int trap, int ins, int a, int *bp, int *sp, int *returnpc) {
	a = 0;
	kernel_task_current[TASK_EXCLUSIVE] = 0;
	trap_exit();
}

// int __user_start_c4r(int argc, char **argv);
// Start a .c4r file from a user process.
// argv is copied based on argc.
// return value is the process id, or 0 if failure.
static void op_user_start_c4r (int trap, int ins, int a, int *bp, int *sp, int *returnpc) {
	int argc;
	char **argv;
	int privileges;
	char *name;
	int *t;

	argc = sp[1];
	argv = (char **)sp[2];
	name = (char *)sp[3];
	privileges = sp[4];
	// Remove kernel privileges if not a kernel task
	if (!(kernel_task_current[TASK_PRIVS] & PRIV_KERNEL))
		privileges = privileges & ~(PRIV_KERNEL);
	// printf("op_user_start_c4r: argc: %d, argv:\n", argc);
	t = start_task_builtin((int *)&task_loadc4r, argc, argv, name, privileges);
	if (!t) {
		printf("op_user_start_c4r: failed to start builtin\n");
		a = 0;
	} else {
		// printf("op_user_start_c4r: builtin started, task id = %d\n", t[TASK_ID]);
		a = t[TASK_ID];
	}
	trap_exit();
}

// int *signal (int sig, int *handler)
// Install a signal handler
static void op_user_signal (int trap, int ins, int a, int *bp, int *sp, int *returnpc) {
	int *sigh, sig;
	sig = sp[1];
	if (sig < 0 || sig > SIGNAL_MAX) {
		printf("c4ke: signal(%d, 0x%X) - invalid signal provided\n");
	} else {
		// printf("c4ke: install user signal handler, sig %d @ 0x%x\n", sp[1], sp[2]);
		sigh = (int *)kernel_task_current[TASK_SIGHANDLERS];
		sigh = sigh + (sig * SIGH__Sz);
		sigh[SIGH_ADDRESS] = sp[2];
	}
	trap_exit();
}

// internal: blank signal handler
static void kernel_task_sighandler_blank () {
}

// internal: set sig handlers for a kernel task.
//           These handlers get set to a blank function.
static void kernel_task_set_sighandlers (int *task) {
	int sig, *sigh;

	sig = 1;
	sigh = (int *)task[TASK_SIGHANDLERS];
	while (sig < SIGMAX) {
		sigh[SIGH_ADDRESS] = (int)&kernel_task_sighandler_blank;
		sigh = sigh + SIGH__Sz;
		++sig;
	}
}

// int pid ()
// return the current task pid
static void op_user_pid (int trap, int ins, int a, int *bp, int *sp, int *returnpc) {
	a = pid();
	trap_exit();
}

// int pid ()
// return the current task pid
static void op_user_parent (int trap, int ins, int a, int *bp, int *sp, int *returnpc) {
	a = kernel_task_current[TASK_PARENT];
	trap_exit();
}

// void currenttask_update_name (char *name)
// Update the name of the current task
static void op_currenttask_update_name (int trap, int ins, int a, int *bp, int *sp, int *returnpc) {
	currenttask_update_name((char *)sp[1]);
	trap_exit();
}

// int await_message(int timeout (0 = never))
static void op_await_message(int trap, int ins, int a, int *bp, int *sp, int *returnpc) {
	*kernel_task_current = *kernel_task_current | STATE_WAITING;
	kernel_task_current[TASK_WAITSTATE] = WSTATE_MESSAGE;
	kernel_task_current[TASK_WAITARG] = kernel_last_time + sp[1]; // timeout
	++kernel_tasks_waiting;
	trap_exit();
	schedule();
	// TODO: fetch first message or return 0
	a = kernel_task_current[TASK_MBOX];
	// printf("c4ke: op_await_message returning, a = %d\n", a);
}

// int kill(int pid, int signal) => 0 (success) -1 (error)
// Send a signal to a process.
// NOTE: not POSIX-compliant, presently only pid > 0 supported. No group signals
// supported as process groups are not implemented.
// TODO: implement process groups.
static void op_user_kill (int trap, int ins, int a, int *bp, int *sp, int *returnpc) {
	int *t, pid, sig;
	pid = sp[1];
	sig = sp[2];
	// printf("c4ke: kill(%d, %d)\n", pid, sig);
	if (pid < 0) {
		printf("c4ke: kill(%d, %d) - invalid process id given\n", pid, sig);
		a = -1;
	} else if (sig < 0) {
		printf("c4ke: kill(%ld, %ld) - process groups not implemented\n", pid, sig);
		a = -1;
	} else if (sig > SIGNAL_MAX) {
		printf("c4ke: kill(%d, %d) - invalid signal provided\n", pid, sig);
		a = -1;
	} else if (!(t = kernel_task_find_pid(pid))) {
		printf("c4ke: kill(%d, %d) - pid not found\n", pid, sig);
		a = -1;
	} else {
		a = internal_signal(t, sig);
	}
	trap_exit();
}

//
// Instructions per second
//

// Measure the instructions per second.
static int measure_ips () {
	int cycles, last_t, elapsed_t;
	int timefactor;

	// Wait until time moves. Ensures that we get a full second to measure
	// under c4.
	last_t = __time();
	while(__time() == last_t)
		; // do nothing

	// Now measure for one second
	elapsed_t = 0;
	last_t = __time();
	cycles = __c4_cycles();
	timefactor = KERNEL_TFACTOR_SLOW;
	while(elapsed_t < KERNEL_MEASURE_QUICK)
		elapsed_t = __time() - last_t;
	if (elapsed_t < KERNEL_MEASURE_SLOW) // Better timestep than 1000?
		timefactor = KERNEL_TFACTOR_QUICK;
	cycles = __c4_cycles() - cycles;

	return cycles * timefactor;
}

//
// Command line parsing
//
static void show_help () {
	printf("c4ke v%s: C4 Kernel Experiment\n"
	       "          : [-dtmg] [-v nn] [-c nn] [--] [init_file.c4r] [arguments...]\n"
	       "           -d               Enable debug mode\n"
	       "           -t               Enable test tasks\n"
	       "           -m               Disable speed measurement\n"
		   "           -g               Load .c4r symbols by default\n"
	       "           -v nn            Enable verbose mode and set verbosity (0 - 100, default %i)\n"
	       "           -c nn            Set cycle interrupt count (implies -m)\n"
	       "           --               End arguments\n"
	       "           init_file.c4r    File to use as init process (default: %s)\n"
	       "           arguments...     Arguments to pass to init process\n"
	       "           --help -h        Show this help\n"
	       , VERSION(), kernel_verbosity, kernel_default_init);
}

// parse_commandline() => 0 success, 1 failure
static int parse_commandline(int argc, char **argv) {
	int   endargs, endopt;
	char *arg;

	endargs = endopt = 0;

	// Skip first argument
	--argc; ++argv;
	if (!argc) return 0;
	
	///
	/// Attempting to narrow down some funky issues with command line parsing segfaults...
	if (argc < 0) {
		printf("c4ke: parse_commandline() warning: argc underflow (%d)\n", argc);
		return 0;
	}
	if (!argv) {
		printf("c4ke: parse_commandline() warning: argv empty (1)\n");
		return 0;
	} else if (!*argv) {
		printf("c4ke: parse_commandline() warning: argv empty (2)\n");
		return 0;
	}
	///

	// Read arguments
	while(argc) {
		// printf("c4ke: cmdline.%i == '%s'\n", argc, *argv);
		// did argv go away?
		if (!*argv) {
			printf("c4ke BUG: argv gone?\n");
			return 0;
		}
		if (!_strcmp(*argv, "--")) {
			endargs = 1;
		} else if (!_strcmp("--help", *argv) || !_strcmp("-h", *argv)) {
			show_help();
			return 1; // Abort
		} else if (endargs == 0 && **argv == '-') {
			arg = *argv + 1; // skip dash
			endopt = 0;
			while (!endopt && *arg) {
				// Single character flags
				if (*arg == 'd') { // Debug mode
					c4r_debug = 1;
					// kernel_verbosity = 100;
				}
				else if (*arg == 't') enable_test_tasks = 1;
				else if (*arg == 'm') enable_measurement = 0;
				else if (*arg == 'g') kernel_loadc4r_mode = kernel_loadc4r_mode | C4ROPT_SYMBOLS;
				// Flags with options
				else if (*arg == 'v') { // Verbosity
					endopt = 1;
					// grab spec from next argv
					--argc; ++argv;
					if (!argc) {
						printf("c4ke: option '-v' requires an argument, none given\n");
						return 1;
					}
					kernel_verbosity = _atoi(*argv, 10);
					c4r_verbose = kernel_verbosity >= VERB_MED;
				} else if (*arg == 'c') { // Cycle count
					endopt = 1;
					// grab from next argv
					--argc; ++argv;
					if (!argc) {
						printf("c4ke: option '-c' requires an argument, none given\n");
						return 1;
					}
					kernel_cycles_count = _atoi(*argv, 10);
					kernel_cycles_force = 1; // skip minimum cycle count check
					enable_measurement  = 0; // skip measurement
				} else {
					printf("Invalid option '%c'\n", *arg);
					show_help();
					return 1;
				}
				++arg;
			}
		} else {
			// initfile
			kernel_init = *argv;
			kernel_init_argc = argc;
			kernel_init_argv = argv;
			return 0;
		}
		--argc; ++argv;
	}

	return 0;
}

///
// Kernel shutdown procedures
///

static int kernel_finish_tasks_wait (int wait_time) {
	int i, t, *tsk, start_time;

	// track start time
	start_time = __time();
	t = 1; // force at least 1 loop
	while(kernel_shutdown == 1 && t && __time() - start_time < wait_time) {
		schedule();
		i = 1; // skip kernel and idle
		t = 0;
		while(++i < KERN_TASK_COUNT) {
			tsk = kernel_tasks + (TASK__Sz * i);
			if (kernel_is_task_running(tsk)) {
				++t; // track tasks needing to terminate
			}
		}
	}

	return t;
}

static int kernel_finish_tasks_send_all (int signal) {
	int i, *tsk;
	int remain;

	i = 1; // skip kernel and idle
	remain = 0;
	while(++i < KERN_TASK_COUNT) {
		tsk = kernel_tasks + (TASK__Sz * i);
		if (kernel_is_task_running(tsk)) {
			printf("c4ke: pid %d '%s' still running, sending signal %d\n", i, (char *)tsk[TASK_NAME], signal);
			internal_signal(tsk, signal);
			++remain; // track tasks needing to terminate
		}
	}

	return remain;
}

static void kernel_finish_tasks_term () {
	int i, *tsk;
	int remain;

	if (kernel_tasks_running && kernel_verbosity >= VERB_MIN)
		printf("c4ke: sending all processes (%d) the TERM signal...\n", kernel_tasks_running - 2);

	remain = kernel_finish_tasks_send_all(SIGTERM);
	if (!remain) {
		if (kernel_verbosity >= VERB_MIN)
			printf("c4ke: no tasks waiting to finish\n");
		return;
	}

	printf("c4ke: waiting for %d tasks to finish...\n", remain);
	remain = kernel_finish_tasks_wait(KERNEL_FINISH_WAIT_TIME);
	if (!remain) {
		if (kernel_verbosity >= VERB_MIN)
			printf("c4ke: SIGTERM successful, tasks finished\n");
		return;
	}

	printf("c4ke: %d tasks remaining, sending KILL...\n", remain);
	remain = kernel_finish_tasks_send_all(SIGKILL);
	if (!remain) {
		return;
	}
	remain = kernel_finish_tasks_wait(KERNEL_FINISH_WAIT_TIME);
	if (!remain) {
		printf("c4ke: SIGKILL successful, all tasks killed\n");
		return;
	}

	if (kernel_verbosity >= VERB_MIN) {
		printf("c4ke: %d tasks remaining, printing details...\n", remain);
		i = 1; // skip kernel and idle
		while(++i < KERN_TASK_COUNT) {
			tsk = kernel_tasks + (TASK__Sz * i);
			if (*tsk) {
				kernel_print_task(tsk);
			}
		}
	}
}

static void kernel_finish_tasks () {
	int remain;

	if (kernel_tasks_running <= 2) { // kernel and idle only?
		if (kernel_verbosity >= VERB_MIN)
			printf("c4ke: no tasks remaining to terminate\n");
	} else {
		kernel_finish_tasks_term();
	}
}


///
// Other functions
///

// Kernel constructor point, called by load-c4r.
// The C4R structure is passed in, allowing stack traces in the kernel.
static int __attribute__((constructor)) kernel_before_main (int *c4r) {
	if (c4r) {
		kernel_c4r = c4r;
		//printf("c4ke: got kernel c4r structure:\n");
		//c4r_dump_info(kernel_c4r);
	}
}

//
// Kernel main entry point.
//

int main (int argc, char **argv) {
	int t, *tsk, i, init_result;
	char **tmp_argv;
	int start_time, measurement_cycles;

	kernel_last_cycle = __c4_cycles();
	kernel_start_time = __time();

	// Install early trap handler
	last_trap_handler = install_trap_handler((int *)&early_trap_handler);

	critical_path_value = 0;
	critical_path_start();

	///
	// Stage 0: configure initial state, including reading command line arguments.
	///
	readable_int_table = " kMGTPEZYRQ";
	readable_int_max   = 11;
	c4r_verbose = 0;
	c4r_debug = 0;
	kernel_last_time = __time();
	kernel_max_slot = 1;
	kernel_cycles_count = KERNEL_CYCLES_MIN;
	kernel_cycles_force = 0;
	kernel_verbosity = VERB_DEFAULT;
	kernel_loadc4r_mode = C4ROPT_NONE;
	kernel_init = kernel_default_init = "init.c4r";
	kernel_init_argc = 0;
	kernel_init_argv = 0;
	kernel_running = 0;
	enable_test_tasks = 0;
	enable_measurement = 1;
	c4_info = __c4_info();
	if (parse_commandline(argc, argv)) {
		// Abort early
		return -1;
	}
	// TODO: schedule_task_mask unused
	schedule_task_mask = STATE_LOADED | STATE_RUNNING | STATE_WAITING;
	kernel_tasks_unloaded = KERN_TASK_COUNT;
	opcode_TLEV = __opcode("TLEV");
	opcode_PSH = __opcode("PSH");

	// Print the banner
	if (kernel_verbosity >= VERB_MIN)
		printf("\nc4ke v%s starting\n", VERSION());
	
	// Print system info
	if (kernel_verbosity >= VERB_MIN) {
		printf("c4ke: system ");
		print_c4_info();
		printf("\n");
	}

	// To make available, run via load-c4r:
	//  c4m load-c4r.c -- c4ke
	if (kernel_verbosity >= VERB_MED) {
		printf("c4ke: kernel symbols %s\n", kernel_c4r ? "present:" : "unavailable");
		if (kernel_c4r)
			c4r_dump_info(kernel_c4r);
	}

	// This removes any cycle interrupt handler (saving the old value), so that
	// initialization cannot be interrupted.
	old_ih_cycle_handler = __c4_configure(C4KE_CONF_CYCLE_INTERRUPT_HANDLER, 0);
	old_ih_cycle_interval = __c4_configure(C4KE_CONF_CYCLE_INTERRUPT_INTERVAL, 0);
	if (old_ih_cycle_handler || kernel_verbosity >= VERB_MAX) {
		printf("c4ke: clear cycle interrupt handler, old @ 0x%lx\n", old_ih_cycle_handler);
		printf("c4ke: clear old cycle interrupt interval @ %ld\n", old_ih_cycle_interval);
	}

	// Measure instructions per second
	//  - disable for faster startup using '-m'
	if (enable_measurement) {
		if (kernel_verbosity >= VERB_MED)
			printf("c4ke: measuring instructions per second, if this step gets stuck, pass -a to c4m...\n");
		measurement_cycles = __c4_cycles();
		kernel_ips = measure_ips();
		measurement_cycles = __c4_cycles() - measurement_cycles;
		if (kernel_verbosity >= VERB_MED) {
			printf("c4ke: instructions per second roughly: ");
			print_int_readable(kernel_ips);
			printf(", measurement cycles: ");
			print_int_readable(measurement_cycles);
			printf("\n");
		}
		// Find a suitable IH_MAX_CYCLES_PER_SECOND
		t = IH_MAX_CYCLES_PER_SECOND;
		i = kernel_ips;
		kernel_cycles_count = i / 10; // if not fast enough, below loop will not run
		// TODO: calibration loop removed
		if (0) {
			while (i > 0) {
				if (i / t > KERNEL_CYCLES_MIN) {
					kernel_cycles_count = i = kernel_ips / t;
				} else {
					i = 0;
				}
				//printf("c4ke: i = %d, t = %d, kcc = %d\n", i, t, kernel_cycles_count);
				i = i / 10;
				t = t * 2;
			}
			if (kernel_verbosity >= VERB_MAX)
				printf("c4ke: after measurement, i = %d, t = %d, kcc = %d\n",
					   i, t, kernel_cycles_count);
		}
	}
	if (!kernel_cycles_force && kernel_cycles_count < KERNEL_CYCLES_MIN)
		kernel_cycles_count = KERNEL_CYCLES_MIN;
	if (kernel_verbosity >= VERB_MIN) {
		printf("c4ke: setting cycle interrupt to %ld (", kernel_cycles_count);
		print_int_readable(kernel_cycles_count);
		printf(") cycles\n");
	}
	kernel_cycles_base = kernel_cycles_count;
	// Ensure SIGRTMAX is in range of SIGNAL_MAX
	if (SIGNAL_MAX < SIGRTMAX)
		printf("c4ke: WARN: SIGNAL_MAX(%d) < SIGRTMAX(%d)\n", SIGNAL_MAX, SIGRTMAX);
	// Initialize extensions if not already done
	// TODO: the return value is not checked
	kext_initialize();
	if (kernel_verbosity >= VERB_MED)
		printf("c4ke: kext_initialize with %d extensions at 0x%lx\n", kernel_ext_count, kernel_extensions);
	// Run the init functions of extensions
	// TODO: return value not checked
	if (kernel_ext_count) {
		if (kernel_verbosity >= VERB_MED)
			printf("c4ke: running kernel extensions init for %d extensions...\n", kernel_ext_count);
		kext_run_all(KEXT_INIT);
	}

	///
	// Stage 1: allocate kernel memory
	// - Allocate custom opcodes table and kernel tasks table.
	// - Initialize kernel extensions data if not already done by a constructor
	///
	if (!(custom_opcodes = malloc((t = sizeof(int) * CO_MAX)))) {
		printf("Unable to allocate %d bytes for custom opcode vector\n", t);
		return -1;
	}
	memset(custom_opcodes, 0, t);
	if (kernel_verbosity >= VERB_MED)
		printf("c4ke: allocated %d (0x%x) bytes for custom_opcodes\n", t, t);

	// TODO: is 1 + kern task count correct? fixes a bad read
	if (!(kernel_tasks = malloc((t = sizeof(int) * TASK__Sz * (1 + KERN_TASK_COUNT))))) {
		printf("Unable to allocate %d bytes for %d tasks\n", t, KERN_TASK_COUNT);
		return -2;
	}
	memset(kernel_tasks, 0, t);
	if (kernel_verbosity >= VERB_MED)
		printf("c4ke: allocated %d (0x%x) bytes for kernel tasks, %d tasks max, %d bytes each, %d bytes ext data\n",
		       t, t, KERN_TASK_COUNT, TASK__Sz * sizeof(int), kernel_task_extdata_size);

	///
	// Stage 2: setup opcode handlers
	// - These allow us to extend C4(m) functionality even further by directly
	//   modifying register values.
	// - Call the init functions of all kernel extensions.
	///
	// Install the bare necesseties requires to run the kernel
	install_custom_opcode(OP_REQUEST_SYMBOL, (int *)&op_request_symbol);
	install_custom_opcode(OP_SCHEDULE, (int *)&op_schedule);
	// printf("c4ke: installed OP_SCHEDULE using handler 0x%x\n", (int *)&op_schedule);
	install_custom_opcode(OP_TASK_FINISH, (int *)&op_task_finish);
	// printf("c4ke: installed OP_TASK_FINISH using handler 0x%x\n", (int *)&op_task_finish);
	install_custom_opcode(OP_TASK_EXIT, (int *)&op_task_exit);
	install_custom_opcode(OP_TASK_FOCUS, (int *)&op_task_focus);
	install_custom_opcode(OP_PEEK_BP, (int *)&op_peek_bp);
	install_custom_opcode(OP_PEEK_SP, (int *)&op_peek_sp);
	install_custom_opcode(OP_C4INFO, (int *)&op_c4info);
	install_custom_opcode(OP_AWAIT_PID, (int *)&op_await_pid);
	install_custom_opcode(OP_USER_SLEEP, (int *)&op_user_sleep);
	// Install various functions used by u0.c to communicate with the kernel
	install_custom_opcode(OP_TASK_CYCLES, (int *)&op_task_cycles);
	install_custom_opcode(OP_SHUTDOWN, (int *)&op_shutdown);
	install_custom_opcode(OP_HALT, (int *)&op_halt);
	// printf("c4ke: installed OP_HALT using handler 0x%x\n", (int *)&op_halt);
	install_custom_opcode(OP_TIME, (int *)&op_time);
	//install_custom_opcode(OP_KERN_PRINT_TASK_STATE, (int *)&op_kern_print_task_state);
	//install_custom_opcode(OP_KERN_GETLEN_TASK_STATE, (int *)&op_kern_getlen_task_state);
	install_custom_opcode(OP_KERN_TASK_CURRENT_ID, (int *)&op_kern_task_current_id);
	install_custom_opcode(OP_KERN_TASK_RUNNING, (int *)&op_kern_task_running);
	install_custom_opcode(OP_KERN_TASK_COUNT, (int *)&op_kern_task_count);
	install_custom_opcode(OP_KERN_TASKS_MAX, (int *)&op_kern_tasks_max);
	install_custom_opcode(OP_KERN_TASKS_EXPORT, (int *)&op_kern_tasks_export);
	install_custom_opcode(OP_KERN_TASKS_EXPORT_UPDATE, (int *)&op_kern_tasks_export_update);
	install_custom_opcode(OP_KERN_TASKS_EXPORT_FREE, (int *)&op_kern_tasks_export_free);
	install_custom_opcode(OP_KERN_TASKS_RUNNING, (int *)&op_kern_tasks_running);
	install_custom_opcode(OP_USER_START_C4R, (int *)&op_user_start_c4r);
	install_custom_opcode(OP_USER_PID, (int *)&op_user_pid);
	install_custom_opcode(OP_USER_PARENT, (int *)&op_user_parent);
	install_custom_opcode(OP_USER_SIGNAL, (int *)&op_user_signal);
	install_custom_opcode(OP_USER_KILL, (int *)&op_user_kill);
	install_custom_opcode(OP_CURRENTTASK_UPDATE_NAME, (int *)&op_currenttask_update_name);
	install_custom_opcode(OP_AWAIT_MESSAGE, (int *)&op_await_message);
	install_custom_opcode(OP_DEBUG_PRINTSTACK, (int *)&op_debug_printstack);
	install_custom_opcode(OP_DEBUG_KERNELSTATE, (int *)&op_debug_kernelstate);
	install_custom_opcode(OP_KERN_REQUEST_EXCLUSIVE, (int *)&op_request_exclusive);
	install_custom_opcode(OP_KERN_RELEASE_EXCLUSIVE, (int *)&op_release_exclusive);

	///
	// Stage 3: setup tasks
	// The builtin kernel task as well as the idle task / reaper are essential.
	// If the '-t' option is given, two test print loop tasks are started.
	// The init process, defaulting to 'init', is started here.
	// Note that "started" means a task with entry task_loadc4r is created. That
	// task may take some time to load and patch the .c4r file given.
	///

	// Kernel task, which is the currently executing code. The kernel calls await_pid()
	// on the init task, and will be woken when the init process exits or something signals
	// the kernel to shut down (eg, the signal handler.)
	// On a task switch, time (cycles) spent searching for the next task to run,
	// and saving/loading register state, are assigned to this kernel task to
	// gain an understanding of how much work the kernel is doing.
	if (kernel_verbosity >= VERB_MAX)
		printf("c4ke: creating kernel task...\n");
	kernel_task_current = kernel_tasks;
	kernel_task_current[TASK_ID]     = 0;
	kernel_task_current[TASK_NICE]   = kernel_task_current[TASK_NICE_BASE] = 0;
	kernel_task_current[TASK_STATE]  = STATE_LOADED;
	kernel_task_current[TASK_REG_A]  = 0;
	kernel_task_current[TASK_REG_BP] = __c4_opcode(OP_PEEK_BP); //0;
	kernel_task_current[TASK_REG_SP] = 0;
	kernel_task_current[TASK_REG_PC] = 0;
	kernel_task_current[TASK_BASE]   = 0; // no base to free
	kernel_task_current[TASK_PRIVS]  = PRIV_KERNEL;
	kernel_task_current[TASK_SIGHANDLERS] = (int)malloc((t = sizeof(int) * SIGH__Sz * SIGNAL_MAX));
	if (!kernel_task_current[TASK_SIGHANDLERS]) {
		printf("c4ke: unable to allocate kernel sig handler\n");
		return -1;
	}
	kernel_task_set_sighandlers(kernel_task_current);
	kernel_task_current[TASK_SIGPENDING] = 0;
	kernel_task_current[TASK_ARGC]  = kernel_task_current[TASK_ARGV] = 0;
	kernel_task_current[TASK_ARGV_DATA] = 0;
	kernel_task_current[TASK_PARENT] = 0;
	kernel_task_current[TASK_WAITSTATE] = kernel_task_current[TASK_WAITARG] = 0;
	kernel_task_current[TASK_ENTRY] = 0;
	kernel_task_current[TASK_EXIT_CODE] = 0;
	kernel_task_current[TASK_TIMEMS] = 0;
	kernel_task_current[TASK_CYCLES] = __c4_cycles();
	kernel_task_current[TASK_TRAPS] = 0;
	kernel_task_current[TASK_C4R] = 0;
	kernel_task_current[TASK_MBOX] = kernel_task_current[TASK_MBOX_SZ] = 0;
	kernel_task_current[TASK_MBOX_COUNT] = 0;
	kernel_task_current[TASK_NAME]   = (int)k_strcpy_alloc("kernel");
	if (kernel_verbosity >= VERB_MAX)
		printf("c4ke: task name updated\n");
	if (!kernel_task_current[TASK_NAME]) {
		printf("c4ke: string allocation failure\n");
		kernel_task_current[TASK_NAME] = (int)"kernel";
	}
	kernel_task_current[TASK_NAMELEN] = _strlen((char *)kernel_task_current[TASK_NAME]);
	kernel_task_current[TASK_EXTDATA] = 0;
	++kernel_tasks_loaded;
	++kernel_tasks_running;
	if (kernel_verbosity >= VERB_MAX)
		kernel_print_task(kernel_task_current);

	// Allocate a temporary argv for use with start_task_builtin()
	// TODO: Why was tmp_argv so small? (wasn't multiplied)
	if (!(tmp_argv = malloc((t = sizeof(char **) * 32)))) {
		printf("c4ke: Unable to allocate some memory (%ld bytes)\n", t);
		return -4;
	}
	memset(tmp_argv, 0, t);

	// Idle task, essential for task reaping. Also performs system halt via sleep()
	if (kernel_verbosity >= VERB_MAX)
        printf("c4ke: creating idle task..\n");
	tmp_argv[0] = "idle";
	if (!(kernel_task_idle = start_task_builtin((int *)&task_idle, 1, tmp_argv, "kernel/idle", PRIV_KERNEL))) {
		printf("Unable to create idle task!\n");
		return -3;
	}
	kernel_task_set_sighandlers(kernel_task_idle);
	if (kernel_verbosity >= VERB_MED)
		printf("c4ke: kernel_task_idle at 0x%x\n", kernel_task_idle);
	kernel_task_idle[TASK_NICE] = kernel_task_idle[TASK_NICE_BASE] = 20;

	// Run kernel extension start callbacks
	// TODO: return value not checked
	if (kernel_ext_count) {
			if (kernel_verbosity >= VERB_MED)
				printf("c4ke: running kernel extensions start for %d extensions...\n", kernel_ext_count);
		kext_run_all(KEXT_START);
	}

	// Test tasks, using kernel functions as entry points.
	if (enable_test_tasks) {
		tmp_argv[0] = "printloop1";
		if (!(tsk = start_task_builtin((int *)&task_printloop_1, 1, tmp_argv, "printloop1", PRIV_USER))) {
			printf("c4ke: Unable to start first printloop\n");
		} else {
			if (kernel_verbosity >= VERB_MED)
				printf("c4ke: task_print_loop_1 started at 0x%x\n", tsk);
			tsk[TASK_NICE] = tsk[TASK_NICE_BASE] = 20;
		}
		tmp_argv[0] = "printloop2";
		if (!(tsk = start_task_builtin((int *)&task_printloop_2, 1, tmp_argv, "printloop2", PRIV_USER))) {
			printf("c4ke: Unable to start second printloop\n");
		} else {
			if (kernel_verbosity >= VERB_MED)
				printf("c4ke: task_print_loop_2 started at 0x%x\n", tsk);
			tsk[TASK_NICE] = tsk[TASK_NICE_BASE] = 20;
		}
	}

	// Init process
	if (!kernel_init_argv) {
		// No argc or argv given, synthesize
		tmp_argv[0] = kernel_init;
		kernel_init_argc = 1;
		kernel_init_argv = tmp_argv;
	}
    if (kernel_verbosity >= VERB_MED)
        printf("c4ke: kernel_init_argc = %i, kernel_init_argv = 0x%x\n", kernel_init_argc, kernel_init_argv);
	if (!(kernel_init_task = start_task_builtin((int *)&task_loadc4r, kernel_init_argc, kernel_init_argv, "init", PRIV_KERNEL))) {
		// Launch emergency shell external
		printf("c4ke: Unable to start init process, starting emergency shell\n");
		tmp_argv[0] = "eshell.c4r";
		if (!(kernel_init_task = start_task_builtin((int *)&task_loadc4r, 1, tmp_argv, "eshell", PRIV_KERNEL))) {
			printf("c4ke: Unable to start emergency shell\n");
		}
		if (kernel_verbosity >= VERB_MED)
			printf("c4ke: emergency shell started at 0x%x\n", kernel_init_task);
	}
	if (kernel_verbosity >= VERB_MED)
		printf("c4ke: init process '%s' started at 0x%x\n", *kernel_init_argv, kernel_init_task);
	// Set focus to the init task
	kernel_task_focus = kernel_init_task;

	// Get rid of tmp_argv
	free(tmp_argv);

	///
	// Stage 4: begin executing tasks
	// - Install the main opcode trap handler
	// - Install the signal handler
	// - Save some details about the kernel startup state
	// - Configure the cycle interrupt handler and then call await_pid(), which will
	//   sleep the calling code until the init process finishes.
	//   This is the last code executed by the kernel before it begins executing
	//   the tasks it has. All setup is complete and all tasks may now run.
	///

	// Install the main trap handler
	install_trap_handler((int *)&trap_handler);
	if (kernel_verbosity >= VERB_MAX)
		printf("c4ke: installed trap handler at 0x%lx, previous = 0x%lx\n",
	           (int *)&trap_handler, last_trap_handler);

	if (kernel_verbosity >= VERB_MAX)
		printf("c4ke: configure signal handlers...\n");
	old_sig_int  = __c4_signal(__c4_sigint(), (int *)&signal_forwarder);
	// old_sig_segv = __c4_signal(SIGSEGV, (int *)&sig_segv_handler);
	if (kernel_verbosity >= VERB_MAX)
		printf("c4ke: old sig int handler: 0x%lx\n", old_sig_int);
	if (kernel_verbosity >= VERB_MAX) {
		printf("c4ke: configuring interrupts...\n");
		printf("c4ke: configure cycle interrupt handler to 0x%lx\n", (int *)&ih_cycle);
	}
	__c4_configure(C4KE_CONF_CYCLE_INTERRUPT_HANDLER, (int *)&ih_cycle);
	if (kernel_verbosity >= VERB_MIN)
		printf("c4ke: configure cycle interrupt interval to %d\n", kernel_cycles_count);
	critical_path_end();
	kernel_schedule_time = __time();
	// Save cycle count
	kernel_last_cycle = __c4_cycles() - measurement_cycles;
	kernel_tasks[TASK_CYCLES] = kernel_last_cycle;
	if (kernel_verbosity >= VERB_MIN) {
		printf("c4ke: Kernel ready in %ldms after ", kernel_schedule_time - kernel_start_time);
		print_int_readable(kernel_last_cycle);
		printf(" cycles, entering task scheduling...\n\n");
	}
	kernel_last_cycle = __c4_cycles();
	__c4_configure(C4KE_CONF_CYCLE_INTERRUPT_INTERVAL, kernel_cycles_count);
	// No longer in early boot
	kernel_running = 1;

	//
	// Setup kernel to wait on init task to finish.
	// This calls schedule() which transfers control to other tasks.
	// It will not return until the init task completes.
	//
	init_result = await_pid(kernel_init_task[TASK_ID]);

	///
	// Stage 5: begin shutdown
	// The init process has ended, begin kernel shutdown.
	// First sends TERM signal to tasks, followed by KILL after a delay.
	///
	kernel_shutdown_time = __time();
	kernel_shutdown = 1;
	if (kernel_verbosity >= VERB_MIN)
		printf("c4ke: init task completed with exit code %d, settling...\n", init_result);
	__c4_opcode(100, OP_USER_SLEEP);
	if (kernel_verbosity >= VERB_MIN)
		printf("c4ke: shutting down...\n");
	kernel_finish_tasks();

	///
	// Stage 6: stop multitasking and finish the kernel.
	// Once satisfied all tasks are complete, removes the cycle interrupt handler
	// and clears all tasks.
	// Getting interrupted at this stage would be bad, so ensure any cleanup code
	// only runs after the cycle interrupt is disabled.
	// Before exiting, old cycle interrupt, trap handlers, and signal handlers
	// are restored.
	///
	if (kernel_ext_count) {
		// Run kernel extensions for shutdown
			if (kernel_verbosity >= VERB_MED)
			printf("c4ke: running kernel extensions shutdown for %d extensions...\n", kernel_ext_count);
		kext_run_all(KEXT_SHUTDOWN);
	}
	if (kernel_verbosity >= VERB_MED)
		printf("c4ke: halting cycle interrupt...\n");
	__c4_configure(C4KE_CONF_CYCLE_INTERRUPT_INTERVAL, 0);
	if (kernel_verbosity >= VERB_MED)
		printf("c4ke: cleaning up all tasks...\n");
	i = 1; // Skip kernel task (cleaned later)
	while (++i < KERN_TASK_COUNT) {
		kernel_clean_task(kernel_tasks + (TASK__Sz * i));
	}
	if (kernel_verbosity >= VERB_MAX)
		printf("c4ke: tasks shutdown\n");
	if (kernel_verbosity >= VERB_MAX)
		printf("c4ke: cleaning up kernel task\n");
	kernel_clean_task(kernel_tasks);
	if (last_trap_handler) {
		if (kernel_verbosity >= VERB_MIN)
			printf("c4ke: unloading trap handler, restoring 0x%lx\n", last_trap_handler);
		install_trap_handler(last_trap_handler);
	}
	if (kernel_verbosity >= VERB_MAX)
		printf("c4ke: unloading memory\n");
	free(custom_opcodes);
	free(kernel_tasks);
	free(kernel_extensions);
	if (old_ih_cycle_handler) {
		if (kernel_verbosity >= VERB_MED)
			printf("c4ke: restoring old cycle interrupt handler @ 0x%lx\n", old_ih_cycle_handler);
		__c4_configure(C4KE_CONF_CYCLE_INTERRUPT_HANDLER, old_ih_cycle_handler);
	}
	if (old_ih_cycle_interval) {
		if (kernel_verbosity >= VERB_MED)
			printf("c4ke: restoring old cycle interrupt interval @ %ld\n", old_ih_cycle_interval);
		__c4_configure(C4KE_CONF_CYCLE_INTERRUPT_INTERVAL, old_ih_cycle_interval);
	}
	if (old_sig_int) {
		if (kernel_verbosity >= VERB_MED)
			printf("c4ke: restoring old signal handler @ 0x%lx\n", old_sig_int);
		__c4_signal(__c4_sigint(), old_sig_int);
	}
	if (kernel_verbosity >= VERB_MED) {
		t = __time();
		printf("c4ke: clean shutdown in ");
		print_time_readable(t - kernel_shutdown_time);
		printf("s, init process ran for ");
		print_time_readable(kernel_shutdown_time - kernel_schedule_time);
		printf("s, total C4KE runtime ");
		print_time_readable(t - kernel_start_time);
		printf("s\nHave a nice day :)\n");
	}

	return init_result;
}


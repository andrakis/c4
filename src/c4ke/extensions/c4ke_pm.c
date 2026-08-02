// C4KE protected mode support extension.
// STATUS: Shelved, makes the kernel slow and unstable.
//
// Handles protected mode violations for system calls:
//  o Output (PUTS, PRTF)
//  o File IO (OPEN, READ, CLOS), eventually custom opcode WRIT
//  o Memory allocation (MALC, FREE)
//  o eventually all the configure options for c4m and other instructions
//    that could be considered priveledged.
//
// A PM violation occurs when a priveledged instruction (usually performing IO or memory
// management) attempts to be executed by a user priveledge task (PRIV_USER).
// The instruction can then go through the filesystem layer, or a custom memory allocator, etc.
//
// A PM violation will only ever occur in protected mode.
//
// C4m will only be in protected mode when executing a user priveledge process (PRIV_USER).
// It is activated by a trap that puts MODE_PROTECTED into the mode register.
//
// All tasks executing in kernel priveledge mode (PRIV_KERNEL) can use the above
// priveledged instructions as they want.
//
// When a user mode task attempts to execute a priveledged instruction (mode == MODE_PROTECTED),
// a trap is generated (TRAP_PM_VIOLATION) with the offending instruction, and C4m changes mode
// to unprotected (mode == MODE_UNPROTECTED). Since the mode is stored in the trap handler
// parameters, a TLEV will restore protected mode.
//
// The C4KE trap handler jumps (__c4_jmp) to pm_syscall_handler, where the instruction can be
// emulated on a per-process basis (ie, to streams).
// Optionally, mode can be changed in this handler to put the calling task into unprotected mode.
// Task scheduling in C4KE sets the mode before switching to it, if kernel_pm_support is non-zero,
// based on the task priveledge (PRIV_KERNEL sets mode to UNPROTECTED, PRIV_USER sets it to PROTECTED.)
//

#ifndef __C4KE_PM_C
#define __C4KE_PM_C 1

#include <c4ke/config.h>
#include <c4ke/extension.h>

#if CONFIG_ENABLE_PM

// Task extended data structure
enum {
	TED_PM_SYSCALL_INS,     // int, the syscall requested
	TED_PM__Size
};

// SysCall Results
enum {
	SCR_FAIL,               // Syscall failure or unhandled
	SCR_SUCCESS,            // Syscall ran to completion
	SCR_RETRY,              // Syscall needs more time
};

// Syscalls we handle
static int OPEN, READ, CLOS, PUTC, PUTS, PRTF, MALC, FREE, INFO, STRC;

// Local data
static int *kernel_pm_thread;
static int  kernel_pm_extdata_start;
static int  kernel_pm_syscall_pending; // pending count

// PM Dispatcher thread non-stack variables
static int pm_dispatch_thread_run;

// Syscall handler non-stack variables
static char *pm_syscall_handler_ops;
static int  *pm_syscall_handler_ext, pm_syscall_handler_r;

// PM Dispatcher thread dispatcher.
// int pm_dispatch_run(int *task, int *sp, int *pointer_to_accumulator, int *returnpc)
//    => SCR_SUCCESS | SCR_FAIL | SCR_RETRY
// This is the common logic for running all syscalls. It is used by both the
// immediate mode syscalls and the syscall dispatcher thread.
static int pm_dispatch_run (int *t, int ins, int *sp, int *a, int *returnpc) {
	int r;

	//if (0 && !t[TASK_EXCLUSIVE])
	//	printf("c4ke: pm_dispatch_run, using ins %d\n", ins);

	if (ins == OPEN) {
		*a = open((char *)sp[1], *sp);
	} else if (ins == READ) {
		*a = read(sp[2], (char *)sp[1], *sp);
	} else if (ins == CLOS) {
		*a = close(*sp);
	} else if (ins == PUTC) {
		*a = putchar(*((char *)sp));
	//} else if (ins == PUTS) {
	//	// TODO: write *sp string to stream
	//	*a = puts((char *)*sp);
	} else if (ins == PRTF) {
		// TODO: deprecate PRTF, implement it as calls to puts and putc in user library.
		r = returnpc[1]; // grab the number of arguments from the ADJ x following the printf
		t = sp + r;
		// Fix potential access violation by not pushing arguments not given
		if      (r == 1) *a = printf((char*)t[-1]);
		else if (r == 2) *a = printf((char*)t[-1], t[-2]);
		else if (r == 3) *a = printf((char*)t[-1], t[-2], t[-3]);
		else if (r == 4) *a = printf((char*)t[-1], t[-2], t[-3], t[-4]);
		else if (r == 5) *a = printf((char*)t[-1], t[-2], t[-3], t[-4], t[-5]);
		else if (r == 6) *a = printf((char*)t[-1], t[-2], t[-3], t[-4], t[-5], t[-6]);
		else if (r == 7) *a = printf((char*)t[-1], t[-2], t[-3], t[-4], t[-5], t[-6], t[-7]);
		else { printf("c4ke: Too many arguments to printf! (%ld)\n", r); exit(-1); }
	} else if (ins == MALC) {
		// TODO: record allocation details somewhere...
		*a = (int)malloc(*sp);
		if (*a != 0)
			t[TASK_MEM_ALLOC] = t[TASK_MEM_ALLOC] + *sp;
	} else if (ins == FREE) {
		// TODO: update allocation details somewhere...
		free((int *)*sp);
	} else if (ins == INFO) {
		// Provide more info
		*a = __c4_info() | C4I_C4KE;
	} else {
		return SCR_FAIL;
	}

	return SCR_SUCCESS;
}

//
// PM Dispatcher thread signal handler
//
void pm_dispatch_thread_signal_handler (int sig) {
	pm_dispatch_thread_run = 0;
}

//
// PM Dispatcher thread
// Cycles through the task list looking for tasks waiting on a syscall.
// Services the syscall (and possibly others, KERNEL_IO_CAP controls this.)
//
static int pm_dispatch_thread (int argc, char **argv) {
	int pid, i, *t, r, retries, *ext, ins, cap, ms;
	char *ops;

	pid = __c4_opcode(OP_USER_PID);
	pm_dispatch_thread_run = 1;
	ops = __c4_ops_list();

	//__c4_opcode((int *)&pm_dispatch_thread_signal_handler, SIGTERM, OP_USER_SIGNAL);
	ksignal((int *)&pm_dispatch_thread_signal_handler, SIGTERM);

	// printf("c4ke_pm: dispatch thread started as pid %d\n", pid);

	// Main loop
	retries = 0;
	while (pm_dispatch_thread_run) {
		critical_path_start();
		// printf("c4ke_pm: worker running, %d pending...\n", kernel_pm_syscall_pending);

		// Loop through and service all pending syscalls
		// TODO: don't do too much here?
		i = 0;
		t = kernel_tasks + (TASK__Sz * 2); // Skip kernel and idle tasks
		cap = KERNEL_IO_CAP; // How many items to service per loop
		//cap = kernel_pm_syscall_pending;
		// critical_path_start();
		ms = kernel_max_slot + 1;
		while (cap && kernel_pm_syscall_pending && ++i < KERN_TASK_COUNT) {
		// while (kernel_pm_syscall_pending && ++i <= ms) {
		// while (kernel_pm_syscall_pending && ++i <= KERN_TASK_COUNT) {
		// while (kernel_pm_syscall_pending && ++i <= KERN_TASK_COUNT) {
			// critical_path_start();
			if (t[TASK_STATE] & STATE_WAITING && t[TASK_WAITSTATE] == WSTATE_SYSCALL && t[TASK_WAITARG] == 0) {
				// Run the syscall. If it returns SYSCALL_OK, update wait state.
				// Otherwise, it needs more time.
				ext = (int *)(t[TASK_EXTDATA] + kernel_pm_extdata_start);
				r = pm_dispatch_run(t, ext[TED_PM_SYSCALL_INS], (int *)t[TASK_REG_SP], &t[TASK_REG_A], (int *)t[TASK_REG_PC]);
				if (r == SCR_SUCCESS) {
					// Mark syscall complete
					t[TASK_WAITARG] = 1;
					--kernel_pm_syscall_pending;
					--cap;
				} else if (r == SCR_RETRY) {
					printf("c4ke_pm: retry not supported yet\n");
					++retries;
					--cap;
				} else if (r == SCR_FAIL) {
					ins = ext[TED_PM_SYSCALL_INS];
					printf("c4ke_pm: dispatch failure for instruction %d (%.4s) in dispatcher thread:\n",
					       ins, &ops[ins * 5]);
					--kernel_pm_syscall_pending;
					// Kill task TODO: make this a function
					kernel_print_task(t);
					kernel_task_finish(t);
					t[TASK_STATE] = STATE_ZOMBIE;
					++kernel_tasks_zombie;
					--cap;
				}
			}
			//critical_path_end();
			t = t + TASK__Sz;
		}
		critical_path_end();

		//printf("c4ke_pm: worker sleeping...\n");

		//if (kernel_pm_syscall_pending || retries > 0) {
		//	retries = 0;
		//} else {
			// Wait to be awoken, then check all tasks
			// TODO: not waiting?
			// __c4_opcode(0, OP_AWAIT_MESSAGE);
			//__c4_opcode(1000, OP_USER_SLEEP); // or until woken
			//sleep(1000);
		//}
		// if (!kernel_pm_syscall_pending) sleep(100);
		schedule();
	}

	printf("c4ke_pm: shutting down\n");
	return 0;
}

//
// Trap handler for syscalls
//

static void pm_syscall_handler (int trap, int ins, int mode, int a, int *bp, int *sp, int *returnpc) {
	// Don't use any stack variables here.

	// Very basic syscall emulator...basically a pass-thru.
	// If in exclusive mode, does the operation immediately.
	// Otherwise, the details are saved to task extended data and the
	// pm_dispatch_thread does the actual operation.
	// TODO: print to streams, track mallocations, frees, etc

	// printf("Syscall handler: T%d  I%d(0x%X) mode%d\n", trap, ins, ins, mode);
	// printf("  SP=0x%X  BP=0x%X  ReturnPC=0x%X\n", sp, bp, returnpc);

	//if (kernel_is_slow || kernel_task_current[TASK_EXCLUSIVE]) {
	if (kernel_task_current[TASK_EXCLUSIVE]) {
	// TODO: servicing reads immediately due to how slow load-c4r is.
	// if (ins == READ || kernel_task_current[TASK_EXCLUSIVE]) {
	// if (kernel_task_current[TASK_EXCLUSIVE]) {
		// TODO: Legacy immediate mode, mainly to support the fact we don't
		//       have streams, and `ps` output gets all muddled.
		if ((pm_syscall_handler_r = pm_dispatch_run(kernel_task_current, ins, sp, &a, returnpc)) != SCR_SUCCESS) {
			printf("c4ke_pm: dispatch failure in immediate mode: result %d\n", pm_syscall_handler_r);
			// Lookup instruction and provide information. Then terminate the task.
			printf("c4ke: BUG: syscall handler for instruction %d (%.4s) missing!\n",
			       ins, &pm_syscall_handler_ops[ins * 5]);
			__c4_jmp((int *)&trap_kill_task_and_schedule + 2);
		}

		trap_exit();
		return;
	}

	// Dispatch to the worker thread
	// Increment pending syscall count
	++kernel_pm_syscall_pending;
	// Set wait state
	//printf("c4ke_pm: setting wait state...\n");
	kernel_task_setwait(kernel_task_current, WSTATE_SYSCALL, 0); // 0 for not finished
	// Save syscall data
	//printf("c4ke_pm: saving syscall data...\n");
	pm_syscall_handler_ext = (int *)(kernel_task_current[TASK_EXTDATA] + kernel_pm_extdata_start);
	pm_syscall_handler_ext[TED_PM_SYSCALL_INS] = ins;
	// Switch to the kernel pm thread
	//printf("c4ke_pm: switching to worker thread 0x%lx\n", kernel_pm_thread);
	trap_schedule_in_trap_next = kernel_pm_thread;
	__c4_jmp((int *)&trap_schedule_in_trap + 2);

	printf("c4ke_pm: BUG: left control of syscall trap handler!\n");
	exit(-3);
}

static int pm_init () {
	char **argv;
	if (kernel_pm_support) {
		// Reserve data in the ext data section
		kernel_pm_extdata_start = kernel_task_extdata_size;
		kernel_task_extdata_size = kernel_task_extdata_size + (sizeof(int) * TED_PM__Size);
		pm_syscall_handler_ops = __c4_ops_list();
	} else if (kernel_verbosity >= VERB_MED) printf("c4ke: protected mode not available.\n");
	return kernel_pm_support ? KXERR_NONE : KXERR_FAIL;
}

static int pm_start () {
	char **argv;
	if (kernel_verbosity >= VERB_MED) {
		if (kernel_pm_support)
			printf("c4ke: protected mode extension enabled\n");
	}
	if (kernel_pm_support) {
		kernel_syscall_handler = ((int *)&pm_syscall_handler) + 2; // skip ENT x
		kernel_syscall_handler_stack = *(kernel_syscall_handler - 1) * -1;
		if (kernel_verbosity >= VERB_MAX) {
			printf("c4ke: protected mode handler: 0x%x\n", &pm_syscall_handler);
			printf("c4ke: protected mode handler stack size: %d\n", kernel_syscall_handler_stack);
		}
		// Request opcodes
		OPEN = __opcode("OPEN");
		READ = __opcode("READ");
		CLOS = __opcode("CLOS");
		PUTC = __opcode("PUTC");
		PUTS = __opcode("PUTS");
		PRTF = __opcode("PRTF");
		MALC = __opcode("MALC");
		FREE = __opcode("FREE");
		INFO = __opcode("INFO");
		STRC = __opcode("STRC");
		if (kernel_verbosity >= VERB_MED) {
			printf("c4ke: handled syscalls: OPEN(%d) READ(%d) CLOS(%d) ", OPEN, READ, CLOS);
			printf("PUTC(%d) PUTS(%d) PRTF(%d) ", PUTC, PUTS, PRTF);
			printf("MALC(%d) FREE(%d) INFO(%d) ", MALC, FREE, INFO);
			printf("STRC(%d)\n", STRC);
		}
		kernel_pm_thread = 0;
		if ((argv = malloc(sizeof(char **) * 32))) {
			argv[0] = "io";
			argv[1] = "";
			if (!(kernel_pm_thread = start_task_builtin((int *)&pm_dispatch_thread, 1, argv, "kernel/io", PRIV_KERNEL))) {
				printf("c4ke: pm support unable to create kernel/io thread\n");
			} else {
				// Set task to run with high priority
				kernel_pm_thread[TASK_NICE] = kernel_pm_thread[TASK_NICE_BASE] = 1;
			}
		} else {
			printf("c4ke: pm support unable to allocate temp argv\n");
		}

		if (!kernel_pm_thread) {
			// Disable pm support
			kernel_pm_support = 0;
		}

		free(argv);
	}
	return KXERR_NONE;
}

static int pm_shutdown () {
	if (kernel_verbosity >= VERB_MAX)
		printf("c4ke: protected mode extension shutdown\n");
	return KXERR_NONE;
}

static int __attribute__((constructor)) pm_constructor () {
	// TODO: event handler no longer needed
	// pm_event_next = kernel_event_handler;
	// kernel_event_handler = (int *) &pm_event;

	kext_register("pm", (int *)&pm_init, (int *)&pm_start, (int *)&pm_shutdown);
	// Detect protected mode here, C4KE's initialization relies on knowing if
	// protected mode is available.
	kernel_pm_support = __c4_info() & C4I_PROT;
}

#endif
#endif

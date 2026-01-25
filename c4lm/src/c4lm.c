/*
 * C4 Lightweight Microkernel, or CALM.
 *
 * A kernel that runs inside C4 and provides multitasking and provides a
 * standard library, but doesn't require altered C4 interpreters like C4m.
 *
 * STATUS: Non-working, on hold.
 * REASON: Cannot task switch more than twice successfully. Maybe it's
 *         because we have no access to the bp register, but modifying
 *         the value on the stack doesn't seem to work gracefully.
 *
 * Must be compiled and loaded with boot.c:
 *   ./c4 boot.c c4lm.c4r
 * Or
 *   make run
 *
 * boot.c can be compiled with:
 *   gcc -E src/load-c4r.c -Iinclude -D__c4cc__=1 -DPURE_C4=1 > boot.c
 * Or
 *   make boot.c
 *
 */

#include <c4lm.h>
#include <c4r.h>
#include <c4lm/syscall.h>

#define C4LM_VERSION "0.01"

// Kernel configuration
enum {
	// Amount of stack words allocate
	CONF_STACK_SIZE = 0xFF,
	// TODO: These options will become obsolete
	CONF_TASK_LIMIT = 32
};

#define NO_LOADC4R_MAIN 1
#include "load-c4r.c"

// Task state
enum {
	TS_FREE,            // Task is not in use
	TS_RUN,             // Task is running
	TS_WAIT,            // Task is waiting
	TS_ZOMBIE           // Task is dead but waiting to be reclaimed
};

// Task structure
enum {
	TASK_STATE,         // See TS_*
	TASK_ID,            // Task/process id
	TASK_PARENT,        // Parent/creator id
	TASK_NAME,          // Name of task
	TASK_NICE,          // Niceness priority
	TASK_NICE_BASE,     // Base value of niceness
	TASK_BASE_BP,       // Allocated BP
	TASK_REG_BP,        // Saved bp register on switch
	TASK_REG_PC,        // Saved stack return pc on switch, or entry
	TASK_EXIT,          // Exit code of task
	TASK_C4R,           // C4R structure pointer
	TASK__Sz            // Size of structure
};

enum { NICE_DEFAULT = 19, NICE_LOW = 19, NICE_MED = 0, NICE_HIGH = -10, NICE_EXEC = -20 };

#define KTASK_CLEAR(t)  memset(t, 0, sizeof(int) * TASK__Sz)

static int *kernel_c4r; // Filled in via constructor
static int *kernel_syscall; // Syscall interface

static char *kernel_boot; // Boot target

// TODO: make a linked list class and implement these
static int *kernel_runlist; // Linked list of tasks that can run now
static int *kernel_waitlist; // ... that are waiting
static int *kernel_zombielist; // ... that have exited and need cleanup
static int *kernel_freelist; // ... that are ready to use

// TODO: REMOVEME: simple big array of tasks for now
static int *kernel_tasks;

static int  kernel_pid_counter;

// References to various tasks
static int *kernel_task, *kernel_task_idle, *kernel_task_current;

///
// Kernel setup and configuration
///
C4R_CONSTRUCTOR(before_main, c4r, syscall) {
	kernel_c4r = c4r;
	kernel_syscall = syscall;
	printf("c4lm: before_main, got c4r @ %lp and syscall @ %lp\n", c4r, syscall);
}

/*
 * setup_defaults: Set kernel variables to initial state before command line
 * arguments are read.
 */
void setup_defaults () {
	kernel_boot = "c4sh";
}

void read_commandline (int argc, char **argv) {
}

void allocate_memory () {
	int size;

	// TODO: linked list instead of task array
	if (!(kernel_tasks = malloc((size = sizeof(int) * TASK__Sz * CONF_TASK_LIMIT)))) {
		printf("c4lm: unable to allocate %ld bytes for task structure\n", size);
		exit(1);
	}
	memset(kernel_tasks, 0, size);
	kernel_task = kernel_tasks;
	kernel_task_idle = kernel_tasks + TASK__Sz;
}

void free_memory () {
	free(kernel_tasks);
}

///
// Task manipulation, just the basics
///
void ktask_print (int *task) {
	printf("Task %d: %s, state %d\n", task[TASK_ID], (char *)task[TASK_NAME], *task);
}
void ktask_print_all () {
	int *task, remain;

	task = kernel_tasks;
	remain = CONF_TASK_LIMIT;
	while (--remain) {
		if (*task)
			ktask_print(task);
		task = task + TASK__Sz;
	}
}
int *ktask_get_free () {
	int *task;
	int  remain;

	task = kernel_tasks + (TASK__Sz * 2); // Skip kernel and idle
	remain = CONF_TASK_LIMIT - 2;
	while (--remain) {
		if (*task == TS_FREE) // Read task state
			return task;
		task = task + TASK__Sz;
	}

	return 0;
}

// Find a task that can run, any task.
int *kt_task, *kt_backup_task, kt_backup_task_nice, kt_nice, kt_remain;
int *ktask_find_runnable_task_now () {

	kt_task = kernel_tasks;
	kt_remain = CONF_TASK_LIMIT;
	kt_backup_task = 0;
	kt_backup_task_nice = 20; // Invalid nice value
	kt_nice = 0;

	while (--kt_remain) {
		if (kt_task == kernel_task_current) {
			// Skip
		} else if (*kt_task == TS_RUN) {
			if (kt_backup_task_nice > ((kt_nice = kt_task[TASK_NICE]))) {
				kt_backup_task = kt_task;
				kt_backup_task_nice = kt_nice;
			}
			if ((kt_task[TASK_NICE] = kt_nice - 1) <= NICE_EXEC) {
				kt_task[TASK_NICE] = kt_task[TASK_NICE_BASE];
				return kt_task;
			}
		}
		printf("Task state %d not correct or current, skipping\n", *kt_task);

		kt_task = kt_task + TASK__Sz;
	}

	if (kt_backup_task)
		return kt_backup_task;

	printf("c4lm: ktask_find_runnable_task_now() failed to find a task\n");
	ktask_print_all();
	exit(2);
}

// Find a task that can run, and try to be fair about it
int *ktask_find_runnable_task_fair () {
	kt_task = kernel_tasks;
	kt_remain = CONF_TASK_LIMIT;
	while (--kt_remain) {
		// TODO: nice/priority. For now this is the same as the _now version.
		if (kt_task == kernel_task_current) {
			// Skip
		} else if (*kt_task == TS_RUN) {
			return kt_task;
		}

		kt_task = kt_task + TASK__Sz;
	}

	return 0;
}

///
// Kernel functions
///

int *target_task;
int *pts_bp, *pts_pc, *pts_task;
void perform_task_switch1 () {
	int *addr;

	pts_task = target_task;
	printf("perform_task_switch from task %lp (%d) to task %lp (%d)\n",
	       kernel_task_current, kernel_task_current[TASK_ID],
	       pts_task, pts_task[TASK_ID]);

	// Use address' address to find bp and pc on stack
	pts_bp = (int *)(&addr + 1);
	pts_pc = (int *)(&addr + 2);

	// Then find the parent and use that
	// TODO: didn't work
	//pts_pc = (int *)*(pts_bp + 1);
	//pts_bp = (int *)*pts_bp;

	// Save old values to task
	kernel_task_current[TASK_REG_BP] = *pts_bp;
	kernel_task_current[TASK_REG_PC] = *pts_pc;
	printf("c4lm/perform_task_switch: saved old bp %lp and pc %lp\n", *pts_bp, *pts_pc);

	// Set new value from new task
	kernel_task_current = pts_task;
	*pts_bp = pts_task[TASK_REG_BP];
	*pts_pc = pts_task[TASK_REG_PC];
	printf("c4lm/perform_task_switch: set new bp %lp and pc %lp\n", *pts_bp, *pts_pc);
}

void perform_task_switch () {
	printf("perform_task_switch() enter\n");
	c4r_print_stacktrace(kernel_c4r, 0, 0, 0);
	perform_task_switch1();
	printf("perform_task_switch() exit\n");
	c4r_print_stacktrace(kernel_c4r, 0, 0, 0);
}

// Schedule to another task, probably because the current one is exiting
void force_task_switch () {
	int *task;

	task = ktask_find_runnable_task_now();
	if (!task) {
		printf("c4lm: no other task to switch to, exiting now\n");
		exit(100);
	}

	target_task = task;
	perform_task_switch();
}

// Schedule to another task if one is available.
int *_task; // TODO: better name
int maybe_try_schedule () {
	if ((_task = ktask_find_runnable_task_fair())) {
		target_task = _task;
		perform_task_switch();
		//printf("maybe_try_schedule() returning success to pid %d\n", kernel_task_current[TASK_ID]);
		return 1; // Found a task to switch to
	}

	// printf("maybe_try_schedule() returning failure to pid %d\n", kernel_task_current[TASK_ID]);
	return 0; // Did not find a task to switch to
}

///
// System calls (syscalls)
///

int syscall_pid () {
	return kernel_task_current[TASK_ID];
}

int syscall_schedule () {
	return maybe_try_schedule();
}

int syscall_time () {
	// TODO
	return 0;
}

void syscall_exit (int code) {
	// Set as zombie
	*kernel_task_current = TS_ZOMBIE;
	kernel_task_current[TASK_EXIT] = code;
	printf("c4lm: pid %d has exited with code %ld\n",
	       kernel_task_current[TASK_ID], code);

	force_task_switch();
}

void setup_syscalls () {
	int size;

	if (!kernel_syscall) {
		if (!(kernel_syscall = malloc((size = sizeof(int) * SYS__Sz)))) {
			printf("c4lm: failed to allocate %ld bytes for syscall interface\n", size);
			exit(1);
		}

		// Populate syscall interface
		*(kernel_syscall + SYS0_PID) = (int)&syscall_pid;
		*(kernel_syscall + SYS0_SCHEDULE) = (int)&syscall_schedule;
		*(kernel_syscall + SYS0_TIME) = (int)&syscall_time;
		*(kernel_syscall + SYS1_EXIT) = (int)&syscall_exit;
	}

	// Set the syscall interface used by load-c4r.c
	c4r_syscall = kernel_syscall;
}

///
// Builtin tasks, started via kstart_task()
///
int task_idle (int argc, char **argv) {
	printf("c4lm: idle starting\n");

	while (1) {
		printf("c4lm/idle: main loop\n");
		syscall_schedule();
	}
}

int task_test_print_a (int argc, char **argv) {
	int pid, i;

	pid = syscall_pid();
	printf("c4lm/test_print_a: started as pid %d\n", pid);

	i = 0;
	while (i++ < 5) {
		printf("pid.%d: print iteration %d\n", pid, i);
		syscall_schedule();
	}

	printf("pid.%d: print iteration finished\n", pid);
}

int task_test_print_b (int argc, char **argv) {
	int pid, i;

	pid = syscall_pid();
	printf("c4lm/test_print_b: started as pid %d\n", pid);

	i = 0;
	while (i++ < 10) {
		printf("pid.%d: print iteration %d\n", pid, i);
		syscall_schedule();
	}

	printf("pid.%d: print iteration finished\n", pid);
}

///
// Task manipulation, part two
///

// Start a new task with the given entry point.
int *kstart_task (char *name, int *entry, int argc, char **argv) {
	int *task, *sp, *bp, *temp;
	// TODO: copy argc and argv
	if (!(task = ktask_get_free()))
		return 0;
	if (!(sp = bp = malloc(CONF_STACK_SIZE * sizeof(int))))
		return 0;

	// Default all task members to 0
	KTASK_CLEAR(task);

	// Stack grows downwards, so move to end of stack
	sp = bp = sp + CONF_STACK_SIZE;
	task[TASK_BASE_BP] = (int)bp;

	// Avoid an issue with C4 where printf can reference arguments past
	// the allocated stack.
	sp = bp = sp - 6;

	// Setup stack such that on return a syscall is made to terminate the task.
	*--sp = (int)&syscall_exit;
	*--sp = JSR;
	*--sp = PSH; // push whatever was returned
	temp = sp; // Save location of JMP to syscall
	*--sp = argc; // TODO: copy this
	*--sp = (int)argv; // TODO: copy this
	*--sp = (int)temp; // Write return location when function exits

	task[TASK_STATE]   = TS_RUN;
	task[TASK_ID]      = ++kernel_pid_counter;
	task[TASK_PARENT]  = 0; // kernel task is creator
	task[TASK_NAME]    = (int)name;
	task[TASK_NICE]    =
	task[TASK_NICE_BASE] = NICE_DEFAULT;
	task[TASK_REG_BP]  = (int)bp;
	task[TASK_REG_PC]  = (int)entry;
	task[TASK_C4R]     = (int)kernel_c4r; // Use kernel c4r for reference

	printf("Task ready, state == %d\n", *task);

	return task;
}

void setup_tasks (int argc, char **argv) {
	int *task;

	// Setup the first task, the kernel task
	kernel_task_current = kernel_task = task = kernel_tasks;
	*task = TS_RUN;
	task[TASK_NAME] = (int)"kernel";
	task[TASK_NICE] =
	task[TASK_NICE_BASE] = NICE_DEFAULT;// NICE_MED;

	// Setup idle task
	if (!(task = kstart_task("kernel/idle", (int *)&task_idle, argc, argv))) {
		printf("c4lm: failed to add idle task\n");
		exit(3);
	}

	// Setup test tasks
	kstart_task("test_a", (int *)&task_test_print_a, argc, argv);
	kstart_task("test_b", (int *)&task_test_print_b, argc, argv);

	ktask_print_all();
}

void test_stacktrace3 () {
	c4r_print_stacktrace(kernel_c4r, 0, 0, 0);
}

void test_stacktrace2 () {
	test_stacktrace3();
}

void test_stacktrace1 () {
	test_stacktrace2();
}

void test_stacktrace () {
	test_stacktrace1();
}

void setup_therest () {
	// Detected by c4r_print_stacktrace
	c4r_c4lm_syscall_exit = (int *)&syscall_exit;
}

int main (int argc, char **argv) {
	int start_time, schedule_time, run_time;

	start_time = syscall_time();
	setup_defaults();
	read_commandline(argc, argv);
	allocate_memory();
	setup_syscalls();
	setup_tasks(argc, argv);
	setup_therest();

	// TODO: testing
	test_stacktrace();

	//return 0;

	schedule_time = syscall_time();
	printf("c4lm version %s ready in %dms, entering scheduling\n", C4LM_VERSION, schedule_time - start_time);
	kernel_task_current = kernel_task;
	while(1) { // TODO: some way of terminating the kernel
		printf("c4lm: main loop task switch\n");
		//force_task_switch();
		syscall_schedule();
	}

	run_time = syscall_time();
	printf("c4lm finished after %ldms\n", run_time - schedule_time);
	free_memory();
	printf("Thank you for using C4LM, have a nice day :)\n");
	return 0;
}

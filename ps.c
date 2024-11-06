// C4 library: ps.c
//           : process listing library
// Renders a (mostly) aligned process listing.
// See top.c for a program that uses this (eshell does too.)
// Can also run by itself - prints a process list and quits.
//
// Requires u0.c
#include "c4.h"
#include "c4m.h"

#include "u0.c"

enum {
	// STATE is first for faster lookup:
	// used *t instead of t[TASK_STATE], saves two multiplications
	TASK_STATE,
	TASK_ID,
	TASK_PARENT,
	TASK_CMD,
	TASK_CMDLEN,
	TASK_PRIORITY,
	TASK_PRIVS,
	TASK_NICE,
	TASK_USAGE_T,
	TASK_USAGE_C,
	TASK_TIMEMS,
	TASK_CYCLES,
	TASK_TRAPS,
	TASK_LAST_TIMEMS,
	TASK_LAST_CYCLES,
	TASK_LAST_TRAPS,
	TASK_DIFF_TIMEMS,
	TASK_DIFF_CYCLES,
	TASK_DIFF_TRAPS,
	TASK__Sz
};

// TODO: add column for privelege mode (- for none, U for user, K for kernel)
// TODO: remove this TODO, this is done...so why don't we have a column for it?
enum {
	COL_PID,
	COL_PARENT,
	COL_CMD,
	COL_PRIORITY,
	COL_NICE,
	COL_STATE,
	COL_USAGE_T,
	COL_USAGE_C,
	COL_TIMEINT,
	COL_TIMETOTAL,
	COL_CYCLESINT,
	COL_CYCLESTOTAL,
	COL_TRAPSINT,
	COL_TRAPSTOTAL,
	COL__Sz
};

enum { __PS_RECALC_COLUMN_FREQUENCY = 2 }; // Only update columns every n displays
int __ps_recalc_column_counter;

static int *__ps_tasks, *__ps_cols, *__ps_kti;
static int __ps_tasks_max, __ps_cols_max;
static int __ps_interval_time;

static int __ps_silent; // suppress output?

int ps_count_running;
int c4_plain;

static char *prio_table;

static char *readable_int_table;
static int   readable_int_max;
static void __attribute__((constructor)) print_int_constructor () {
	readable_int_table = " kMGTPEZYRQ";
	readable_int_max   = 11;
}
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
	if (readable_int_table[table_pos] != ' ') {
		printf("%4ld.%03d %c", n, rem, readable_int_table[table_pos]);
	} else {
		printf("       %3ld", n);
	}
}

void ps () {
	int *t, i, l, c, x;
	char *s;
	char *ps_blank;
	int *ti;
	int *col;
	int task_count, total_usage_t, total_usage_c;
	int cpu_usage_t, cpu_usage_c, cpu_usage_k, cpu_usage_i;
	int cpu_total_c;
	int time_refresh;
	int tasks_loaded, tasks_running, tasks_waiting, tasks_zombie, tasks_free;

	time_refresh = __time();
	kern_tasks_export_update(__ps_kti);
	// TODO: why do we need a + 1 here?
	//       without it we miss printing a process, but not sure why
	tasks_loaded = task_count = __ps_kti[KTI_USED] + 1;

	// load up task information and count time and cycles for totals
	ti = (int *)__ps_kti[KTI_LIST];
	t  = __ps_tasks;
	i = total_usage_t = total_usage_c = cpu_total_c =
	  tasks_running = tasks_waiting = tasks_zombie = 0;
	tasks_free = __ps_kti[KTI_COUNT] - tasks_loaded;
	while (++i < task_count) {
		// Only count tasks that are present towards usage,
		// and only copy data from them if they are present.
		if ((*t = x = ti[KTE_TASK_STATE])) {
			// TODO: reorganise these such that we can do ++*target = ++*source
			t[TASK_ID]          = ti[KTE_TASK_ID];
			t[TASK_PARENT]      = ti[KTE_TASK_PARENT];
			t[TASK_CMD]         = ti[KTE_TASK_NAME];
			t[TASK_CMDLEN]      = ti[KTE_TASK_NAMELEN];
			//t[TASK_PRIORITY]    = ti[KTE_TASK_PRIORITY];
			t[TASK_PRIVS]       = ti[KTE_TASK_PRIVS];
			t[TASK_NICE]        = ti[KTE_TASK_NICE];
			t[TASK_LAST_TIMEMS] = t[TASK_TIMEMS];
			t[TASK_TIMEMS]      = ti[KTE_TASK_TIMEMS];
			t[TASK_LAST_CYCLES] = t[TASK_CYCLES];
			t[TASK_CYCLES]      = ti[KTE_TASK_CYCLES];
			t[TASK_LAST_TRAPS]  = t[TASK_TRAPS];
			t[TASK_TRAPS]       = ti[KTE_TASK_TRAPS];
			t[TASK_DIFF_TIMEMS] = t[TASK_TIMEMS] - t[TASK_LAST_TIMEMS];
			t[TASK_DIFF_CYCLES] = t[TASK_CYCLES] - t[TASK_LAST_CYCLES];
			t[TASK_DIFF_TRAPS]  = t[TASK_TRAPS]  - t[TASK_LAST_TRAPS];
			total_usage_t = total_usage_t + t[TASK_DIFF_TIMEMS];
			total_usage_c = total_usage_c + t[TASK_DIFF_CYCLES];
			cpu_total_c   = cpu_total_c   + t[TASK_DIFF_CYCLES];
			if (x & STATE_WAITING) ++tasks_waiting;
			else if (x & STATE_RUNNING) ++tasks_running;
			else if (x & STATE_ZOMBIE) ++tasks_zombie;
		}
		t = t + TASK__Sz;
		ti = ti + KTE__Sz;
	}
	//printf("ps: task information copied\n");
	//printf("ps: counted rel cycles: %d\n", rel_cycles);
	//printf("ps: total usage %d\n", total_usage);

	// Count up columns for pretty printing
	t = __ps_tasks;
	ti = (int *)__ps_kti[KTI_LIST];
	i = 0;
	// Set default column sizes for columns that resize
	__ps_cols[COL_PID] = 4; // Use at least 4 for column
	__ps_cols[COL_PARENT] = 4;
	__ps_cols[COL_CMD] = 4;
	__ps_cols[COL_TIMEINT] = 4;
	__ps_cols[COL_TIMETOTAL] = 4;
	__ps_cols[COL_CYCLESTOTAL] = 4;
	__ps_cols[COL_TRAPSINT] = 5;
	col = __ps_cols + COL__Sz;
	// These checks fix a very hard to diagnose floating point exception.
	// C4 does not use floating point.
	//if (total_usage_t == 0) total_usage_t = 1;
	++total_usage_t;
	//if (total_usage_c == 0) total_usage_c = 1;
	++total_usage_c;
	cpu_usage_t = cpu_usage_c = cpu_usage_k = cpu_usage_i = 0;
	// Calculate columns and column lengths
	while (++i < task_count) {
		// printf("ps: task %d of %d\n", i - 1, task_count);
		if (*t) {
			//c = 0;
			//if ((s = (char *)t[TASK_CMD])) {
			//	// Either print until first space
			//	// while(*s && *s != ' ') { c++; ++s; }
			//	// Or print the entire thing:
			//	while(*s++) { c++; }
			//}
			col[COL_CMD]         = t[TASK_CMDLEN];
			// printf("ps:   counted CMD as %d ('%s')\n", c, (char *)t[TASK_CMD]);
			col[COL_PID]         = itoa_len(t[TASK_ID]);
			col[COL_PARENT]      = itoa_len(t[TASK_PARENT]);
			// printf("ps:   counted PID as %d\n", col[COL_PID]);
			//col[COL_STATE] = kern_getlen_task_state(t[TASK_STATE]);
			//col[COL_STATE]       = kern_getlen_task_state(t[TASK_STATE]);
			//col[COL_PRIORITY]    = itoa_len(t[TASK_PRIORITY]);
			//col[COL_NICE]        = itoa_len(t[TASK_NICE]);
			// printf("ps:   counted STATE as %d\n", col[COL_STATE]);
			//col[COL_TIMEMS] = itoa_len(t[TASK_TIMEMS]);
			t[TASK_USAGE_T]      = (t[TASK_DIFF_TIMEMS] * 100) / total_usage_t;
			t[TASK_USAGE_C]      = (t[TASK_DIFF_CYCLES] * 100) / total_usage_c;
			//col[COL_USAGE_T]     = itoa_len(t[TASK_USAGE_T]);
			//col[COL_USAGE_C]     = itoa_len(t[TASK_USAGE_C]);
			col[COL_TIMEINT]     = itoa_len(t[TASK_DIFF_TIMEMS]);
			col[COL_TIMETOTAL]   = itoa_len(t[TASK_TIMEMS]);
			// printf("ps:   counted TIMEMS as %d (%d value)\n", col[COL_TIMEMS], t[TASK_TIMEMS]);
			//col[COL_CYCLESINT]   = itoa_len(t[TASK_DIFF_CYCLES]);
			//col[COL_CYCLESTOTAL] = itoa_len(t[TASK_CYCLES]);
			col[COL_TRAPSINT]    = itoa_len(t[TASK_DIFF_TRAPS]);
			//col[COL_TRAPSTOTAL]  = itoa_len(t[TASK_TRAPS]);
			// printf("ps:   counted CYCLES as %d (%d value)\n", col[COL_CYCLES], t[TASK_CYCLES]);
			//col[COL_CYCLES_REL] = itoa_len((t[TASK_CYCLES] * 100) / rel_cycles);
			// printf("ps:   counted CYCLES_REL as %d\n", col[COL_CYCLES_REL]);
			//col[COL_CYCLES_ALL] = itoa_len((t[TASK_CYCLES] * 100) / all_cycles);
			// printf("ps:   counted CYCLES_ALL as %d\n", col[COL_CYCLES_ALL]);
			if (col[COL_PID] > __ps_cols[COL_PID]) __ps_cols[COL_PID] = col[COL_PID];
			if (col[COL_PARENT] > __ps_cols[COL_PARENT]) __ps_cols[COL_PARENT] = col[COL_PARENT];
			if (col[COL_CMD] > __ps_cols[COL_CMD]) __ps_cols[COL_CMD] = col[COL_CMD];
			//if (col[COL_STATE] > __ps_cols[COL_STATE]) __ps_cols[COL_STATE] = col[COL_STATE];
			//if (col[COL_PRIORITY] > __ps_cols[COL_PRIORITY]) __ps_cols[COL_PRIORITY] = col[COL_PRIORITY];
			//if (col[COL_NICE] > __ps_cols[COL_NICE]) __ps_cols[COL_NICE] = col[COL_NICE];
			//if (col[COL_USAGE_T] > __ps_cols[COL_USAGE_T]) __ps_cols[COL_USAGE_T] = col[COL_USAGE_T];
			//if (col[COL_USAGE_C] > __ps_cols[COL_USAGE_C]) __ps_cols[COL_USAGE_C] = col[COL_USAGE_C];
			// if (col[COL_TIMEMS] > __ps_cols[COL_TIMEMS]) __ps_cols[COL_TIMEMS] = col[COL_TIMEMS];
			if (col[COL_TIMEINT] > __ps_cols[COL_TIMEINT]) __ps_cols[COL_TIMEINT] = col[COL_TIMEINT];
			//if (col[COL_TIMETOTAL] > __ps_cols[COL_TIMETOTAL]) __ps_cols[COL_TIMETOTAL] = col[COL_TIMETOTAL];
			//if (col[COL_CYCLESINT] > __ps_cols[COL_CYCLESINT]) __ps_cols[COL_CYCLESINT] = col[COL_CYCLESINT];
			//if (col[COL_CYCLESTOTAL] > __ps_cols[COL_CYCLESTOTAL]) __ps_cols[COL_CYCLESTOTAL] = col[COL_CYCLESTOTAL];
			if (col[COL_TRAPSINT] > __ps_cols[COL_TRAPSINT]) __ps_cols[COL_TRAPSINT] = col[COL_TRAPSINT];
			//if (col[COL_TRAPSTOTAL] > __ps_cols[COL_TRAPSTOTAL]) __ps_cols[COL_TRAPSTOTAL] = col[COL_TRAPSTOTAL];
			// if (col[COL_CYCLES_REL] > __ps_cols[COL_CYCLES_REL]) __ps_cols[COL_CYCLES_REL] = col[COL_CYCLES_REL];
			// if (col[COL_CYCLES_ALL] > __ps_cols[COL_CYCLES_ALL]) __ps_cols[COL_CYCLES_ALL] = col[COL_CYCLES_ALL];
			// printf("ps:   finished\n");
			// off by 1
			if (i == 1) {
				// kernel task
				cpu_usage_k = t[TASK_DIFF_CYCLES];
				//cpu_usage_t = cpu_usage_t + (t[TASK_DIFF_TIMEMS] * 100);
				//cpu_usage_c = cpu_usage_c + (t[TASK_DIFF_CYCLES] * 100);
			} else if (i == 2) {
				// idle
				cpu_usage_i = t[TASK_DIFF_TIMEMS];
				//cpu_usage_t = cpu_usage_t + (t[TASK_DIFF_TIMEMS] * 100);
				//cpu_usage_c = cpu_usage_c + (t[TASK_DIFF_CYCLES] * 100);
			} else if (*t) { // only consider tasks present
				cpu_usage_t = cpu_usage_t + (t[TASK_DIFF_TIMEMS] * 100);
				cpu_usage_c = cpu_usage_c + (t[TASK_DIFF_CYCLES] * 100);
			}
		} else {
			col[COL_CMD] = 0;
		}
		t = t + TASK__Sz;
		col = col + COL__Sz;
	}
	// printf("ps: column sizes calculated\n");

	cpu_usage_t = cpu_usage_t / total_usage_t;
	cpu_usage_c = cpu_usage_c / total_usage_c;
	cpu_usage_k = (cpu_usage_k * 100) / total_usage_c;
	//cpu_usage_i = (cpu_usage_i * 100) / total_usage_t;

	//if (0) {
	//printf("ps: columns constructed:\n");
	//printf("        PID=%d CMD=%d STATE=%d TIMEMS=%d CYCLES=%d\n",
	//       __ps_cols[COL_PID], __ps_cols[COL_CMD], __ps_cols[COL_STATE], __ps_cols[COL_TIMEMS], __ps_cols[COL_CYCLES]);
	//printf("        CYCLES_REL=%d CYCLES_ALL=%d\n",
	//       __ps_cols[COL_CYCLES_REL], __ps_cols[COL_CYCLES_ALL]);
	//}

	// If silent mode, return after updating interval time
	if (__ps_silent) {
		__ps_interval_time = time_refresh;
		return;
	}

	// Disable cycle interrupt so that printing is not interrupted
	kern_request_exclusive();
	// printf("ps: got __ps_kti @ 0x%x, count = %d\n", __ps_kti, __ps_kti[KTI_COUNT]);
	// Print task counts
	printf("\nTasks: %3d total, %d running, %d waiting, %d zombie, %d free\n",
	       tasks_loaded - 1, tasks_running, tasks_waiting, tasks_zombie, tasks_free);
	// Print usage
	if (c4_plain)
		printf("Usage: %3d%% (%d%% kernel cycles, %dms idle time, ",
		       cpu_usage_c, cpu_usage_k, cpu_usage_i);
	else
		printf("Usage: %3d%% (%d%% kernel cycles, %dms idle time, ",
		       cpu_usage_t, cpu_usage_k, cpu_usage_i);
	if (__ps_interval_time == 0) {
		print_int_readable(cpu_total_c);
		printf(" cycles/s, interval time unknown (first run))\n");
	} else {
		l = time_refresh - __ps_interval_time;
		if (l) {
			// We get overflows on 32bit systems if we just use l as is, but we don't want
			// to lose too much precision on the calculation.
			print_int_readable(cpu_total_c * 100 / (l / 10));
		} else {
			printf("%d", l);
		}
		printf(" cycles/s, interval time %ldms)\n", l);
	}
	__ps_interval_time = time_refresh;
	// Print header
	// Print columns
	// Don't ask why these and the numbers in the loop below work.
	// I've tried to make it nicer but it eludes me.
	printf("  PID%*sPARENT%*s", __ps_cols[COL_PID] - 3, " ", __ps_cols[COL_PARENT] - 3, " ");
	printf(" CMD%*sSTATE ", __ps_cols[COL_CMD], " ");
	// TODO: PRIO replaced by PRIV
	printf(" PRIV  NI   T%%    C%%  ");
	printf("time/INTER%*s", __ps_cols[COL_TIMEINT] - 3, " ");
	printf("cycles/INTER%*s", __ps_cols[COL_CYCLESINT] - 6, " ");
	printf("traps/INTER%*s", __ps_cols[COL_TRAPSINT] - 3, " ");
	printf("time/TOTAL%*s", __ps_cols[COL_TIMETOTAL] - 4, " ");
	printf("cycles/TOTAL%*s", __ps_cols[COL_CYCLESTOTAL] - 5, " ");
	printf("traps/TOTAL%*s", __ps_cols[COL_TRAPSTOTAL] - 5, " ");
	printf("\n");
	//printf("TIME (ms)%*s", __ps_cols[COL_TIMEMS], " ");
	//printf("CYCLES%*s", __ps_cols[COL_CYCLES], " ");
	//printf("CYCLES REL %%%*sCYCLES ALL %%\n", __ps_cols[COL_CYCLES_REL], " ");

	// Print columns and update running process counter
	ps_count_running = 0;
	col = __ps_cols + COL__Sz;
	t = __ps_tasks;
	i = 0;
	// TODO: rearrange columns so that we can use *ps_cols - *col; ++ps_cols; ++col;
	//       looks worse than *ps_cols++ - *col++ but results in less code as postincrements
	//       result in more code.
	while (++i < task_count) {
		if (*t) {
			// PID and PARENT
			printf(" %*d %*d", __ps_cols[COL_PID], t[TASK_ID], __ps_cols[COL_PARENT], t[TASK_PARENT]);
			// CMD
			printf("%*s%s %*s", __ps_cols[COL_PARENT], " ", (char *)t[TASK_CMD],
			       2 + __ps_cols[COL_CMD] - col[COL_CMD], " ");
			// State
			kern_print_task_state(*t);
			// TASK_PRIVS was TASK_PRIO
			printf("      %c    %2d  %3d%%  %3d%% ", prio_table[t[TASK_PRIVS]], t[TASK_NICE],
			       t[TASK_USAGE_T], t[TASK_USAGE_C]);
			printf(" %d%*s", t[TASK_DIFF_TIMEMS], 6 + __ps_cols[COL_TIMEINT] - col[COL_TIMEINT], " ");
			print_int_readable(t[TASK_DIFF_CYCLES]);
			printf("      %d%*s", t[TASK_DIFF_TRAPS], 5 + __ps_cols[COL_TRAPSINT] - col[COL_TRAPSINT], " ");
			printf("   %ld%*s  ", t[TASK_TIMEMS], 5 + __ps_cols[COL_TIMETOTAL] - col[COL_TIMETOTAL], " ");
			print_int_readable(t[TASK_CYCLES]);
			printf("    %ld%*s\n", t[TASK_TRAPS], 5 + __ps_cols[COL_TRAPSTOTAL] - col[COL_TRAPSTOTAL], " ");
            ++ps_count_running;
		}
		t = t + TASK__Sz;
		col = col + COL__Sz;
	}
	// printf("ps: columns printed\n");
	kern_release_exclusive();
}

int ps_init () {
	int i;

	if (!(__ps_kti = kern_tasks_export())) {
        printf("ps: failed to export tasks\n");
        return -1;
    }

	__ps_tasks_max = __ps_kti[KTI_COUNT];
	__ps_cols_max  = 32;
	c4_plain = __c4_info() & C4I_C4;

	// TODO: memory corruption?
	if (!(__ps_tasks = malloc((i = __ps_tasks_max * TASK__Sz * sizeof(int) * 2)))) {
		//printf("ps: memory allocation failure\n");
		return -1;
	}
	// printf("ps: allocated %d bytes for tasks\n", i);
	memset(__ps_tasks, 0, i);
	if (!(__ps_cols = malloc((i = __ps_cols_max * COL__Sz * (sizeof(int) * 4))))) {
		//printf("ps: malloc for columns failed\n");
		return -1;
	}
	// printf("ps: allocated %d bytes for cols\n", i);
	memset(__ps_cols, 0, i);
	__ps_interval_time = 0;
    // printf("ps: tasks max=%ld, cols max=%ld, kti = 0x%lx\n", __ps_tasks_max, __ps_cols_max, __ps_kti);

	// Set static column sizes (these never change)
	__ps_cols[COL_STATE] = 1;
	__ps_cols[COL_PRIORITY] = 3;
	__ps_cols[COL_NICE] = 3;
	__ps_cols[COL_USAGE_T] = 3;
	__ps_cols[COL_USAGE_C] = 3;
	__ps_cols[COL_CYCLESINT] = 4;
	__ps_cols[COL_TRAPSTOTAL] = 4;

	prio_table = "-UK"; // NONE, USER, KERN

	// TODO: fixes the unknown interval display, but prevents cycles/s working on first run
	// Grab an initial listing
	//__ps_silent = 1;
	//ps();
	//__ps_silent = 0;

	return 0;
}

void ps_uninit () {
	free(__ps_tasks);
	free(__ps_cols);
	kern_tasks_export_free(__ps_kti);
}


#ifndef PS_NOMAIN
int main (int argc, char **argv) {
	if (ps_init()) return -1;
	ps();
	ps_uninit();
	return 0;
}
#endif

// tests/test_timekeeping.c - Different timekeeping methods
//
// Designed to be run under plain c4 with c4m.

#include <fcntl.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>

#include <c4.h> // For gcc compatibility
#include <c4m.h>

int cr_timekeeping; // CRTK_*
enum {
	CRTK_DETECT,    // Detect available modes
	CRTK_BUILTIN,   // A builtin time call
	CRTK_UPTIME,    // Slow: read /proc/uptime
	CRTK_CYCLES,    // Cycle-based timekeeping
};


// C4INFO state
enum {
	C4I_NONE = 0x0,  // No C4 info
	C4I_C4   = 0x1,  // Ultimately running under C4
	C4I_C4M  = 0x2,  // Running under c4m (directly or C4)
	C4I_C4P  = 0x4,  // Running under c4plus
	C4I_C4MJS= 0x8,  // Running under C4M.JS host
	C4I_HRT  = 0x10, // High resolution timer
	C4I_SIG  = 0x20, // Signals supported
	C4I_FLT  = 0x40, // Floating point instruction support
	C4I_PROT = 0x80, // Protected mode support
};

char *readable_int_table;
int   readable_int_max;
void print_int_readable (int n) {
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

//
// Timekeeping function from c4m
//
	int   c4_time_unavailable;
	char *c4_time_buf; // allocated by main
	int   c4_time_unavailable;
	int   c4_time_approx, c4_time_last;
	int   c4_time_altmode;
	enum {
		// Buffer size for reading timefile
		C4_TIME_BUF_SZ = 32,
		// How often to read the timefile
		C4_TIME_APPROX = 1
	};
	// Attempt to read from a time source.
	// This is hacky, it currently reads the file /proc/uptime for
	// a time reference.
	// This is meant to be fast, but it is not very pretty
	// TODO: some systems have different formats for uptime.
	//       On Ubuntu-x64, the first number only updates every second, and the
	//       second number updates every 100ms.
	//       This is reverse on Debian-pi.
	//       Use -a flag to use alternate format.
	int c4_time () {
		int fd, number, r, i;
		char *buf, ch;

		// Don't complain endlessly
		if (c4_time_unavailable)
			return 0;

		if ((fd = open("/proc/uptime", 0)) < 0) {
			printf("c4m: unable to open uptime file\n");
			c4_time_unavailable = 1;
			return 0;
		}

		buf = c4_time_buf;
		if (!buf) {
			printf("c4m: buffer went away\n");
			return 0;
		}
		r = read(fd, buf, C4_TIME_BUF_SZ);
		close(fd);
		if (r < 0) {
			printf("c4m: read returned %d, buf: '%s' (0x%lx)\n", r, buf, buf);
			return 0;
		}

		r = C4_TIME_BUF_SZ;
		if (!c4_time_altmode) {
			// Find first space
			while(*buf++ != ' ') {
				--r;
			}
		}

		number = 0;
		while(--r > 0 && *buf) {
			ch = *buf;
			if (ch == '.' || ch == ' ') {
				if (c4_time_altmode) {
					// Now read hundredths of seconds
					i = 0;
					++buf;
					while (--r > 0 && *buf) {
						if (*buf == ' ') {
							// Convert to milliseconds
							number = (number * 1000) + (i * 10);
							// printf("c4: got number: %ld\n", number);
							return number;
						} else {
							i = (i * 10) + (*buf - '0');
						}
						++buf;
					}
					printf("c4_time overran buffer\n");
					return c4_time_last;
				} else
					return c4_time_last = number * 100;
			}
			number = (number * 10) + (ch - '0');
			++buf;
		}
	}
//
// End timekeeping function from c4m
//

int invoke_c4_time() {
	return c4_time();
}

int invoke_c4_time_pure() {
	return __c4_invoke((int *)&c4_time);
}

int measure_loops, work_mode;
// Measure how long a given count of measure_loops takes, using c4_time.
int measure_cycles (int count) {
	int start_time, end_time, start_cycles, diff;
	start_time = invoke_c4_time();
	start_cycles = __c4_cycles();
	measure_loops = 0;
	while (__c4_cycles() - start_cycles < count) {
		if (work_mode)
			c4_time();
		++measure_loops;
	}
	end_time = invoke_c4_time();
	diff = end_time - start_time;
	// printf("start_time: %ld, end_time: %ld, difference: %ld\n", start_time, end_time, diff);
	return diff;
}

int sync_test_work (int loops) {
	while (--loops)
		loops = loops + (loops ^ loops);
}

int c4_abs (int x) { return x < 0 ? -x : x; }

int sync_test (int cps) {
	int start, curr, max_ms, resync_ms, resync_ms_time;
	int estimated_ms, total_estimated_ms;
	int cycles, cycles_elapsed, diff, loops, min_work_time;
	int cycles_after, target_cps, min_adjustment_threshold;

	start = curr = invoke_c4_time();
	max_ms = 60000;
	resync_ms = resync_ms_time = 1000;
	total_estimated_ms = 0;
	loops = 4;
	min_work_time = 5;
	min_adjustment_threshold = 50; // ms

	while (total_estimated_ms < max_ms) {
		estimated_ms = 0;
		// Ensure we do enough work
		while (estimated_ms < min_work_time) {
			// Do some work and measure it
			cycles = __c4_cycles();
			sync_test_work(loops);
			cycles_after = __c4_cycles();
			cycles_elapsed = (cycles_after - cycles);
			estimated_ms = (cycles_elapsed * 1000) / cps;
			// Rework the loops counter if needed
			if (estimated_ms < 0) {
				printf("Overflow? estimated: %ldms, cycles (before): %ld, cycles (after): %ld, elapsed: %ld\n",
						estimated_ms, cycles, cycles_after, cycles_elapsed);
				loops = loops / 2;
			} else if (estimated_ms < min_work_time) {
				loops = loops * 2;
				printf("increasing loops to %ld as estimated_ms == %ld\n", loops, estimated_ms);
			}
		}
		// Update estimated time
		total_estimated_ms = total_estimated_ms + estimated_ms;
		//printf("total_estimated_ms: %ldms (work took %ldms with %ld loops)\n", total_estimated_ms, estimated_ms, loops);
		// Have we hit the estimated 1 second?
		if ((resync_ms_time = resync_ms_time - estimated_ms) < 0) {
			// Query current time
			resync_ms_time = resync_ms;
			curr = invoke_c4_time();
			diff = (curr - (total_estimated_ms + start));
			// Adjust cycles per second to try to hit closer to real time
			if (c4_abs(diff) < min_adjustment_threshold) {
				// Close enough, no change
				printf("Resync pass, %ldms under threshold %ldms\n", diff, min_adjustment_threshold);
			} else {
				if (diff < 0) {
					// Adjust upwards by 10%
					cps = cps + (cps / 10);
				} else {
					// Adjust downwards by 10%
					cps = cps - (cps / 10);
				}
				printf("Resync, read time: %ldms, expected time: %ldms, difference: %ldms, cps now: ",
						curr, total_estimated_ms + start, diff);
				print_int_readable(cps);
				printf(" (%ld)\n", cps);
			}
			// Adjust estimate according to difference
			total_estimated_ms = total_estimated_ms + diff;
		}
	}

	return 0;
}

int main (int argc, char **argv) {
	int calibration, current_calibration, calibration_loops, measure_ms, total;
	int total_normal, total_altmode;
	int run_total, run_avg;
	int cycles_per_second;
	int factor, saw0, min_time, min_loops;
	int *patch, start;

	readable_int_table = " kMGTPEZYRQ";
	readable_int_max   = 11;
	work_mode = 0;
	calibration = 50;
	total = 0;
	min_time = 500;
	min_loops = 10;
	c4_time_altmode = 0;
	calibration_loops = 2;

	--argc; ++argv;
	if (argc > 0 && **argv == '-' && (*argv)[1] == 'a') { c4_time_altmode = 1; --argc; ++argv; }
	if (argc > 0 && **argv == '-' && (*argv)[1] == 'w') { work_mode = 1; --argc; ++argv; }
	printf("altmode: %d, workmode: %d\n", c4_time_altmode, work_mode);

	if (1 && __c4_info() & C4I_C4) {
		printf("Running under C4, patching invoke_c4_time\n");
		patch = (int *)&invoke_c4_time;
		*patch++ = __opcode("JMP");
		*patch = (int)&invoke_c4_time_pure;
	}

	if (!(c4_time_buf = malloc(C4_TIME_BUF_SZ))) { printf("could not malloc(%d) time buffer\n", C4_TIME_BUF_SZ); return -1; }

	saw0 = 1;
	run_avg = measure_loops = 0;
	start = invoke_c4_time();
	while (saw0 || run_total < min_time || measure_loops < min_loops) {
		run_total = saw0 = 0;
		current_calibration = calibration_loops;
		while (current_calibration--) {
			if ((measure_ms = measure_cycles(calibration)) == 0)
				saw0 = 1;
			printf("test of %ld: %ldms, %ld loops performed\n", calibration, measure_ms, measure_loops);

			run_total = run_total + measure_ms;
		}
		run_avg = run_total / calibration_loops;
		if (saw0 || run_total < min_time || measure_loops < min_loops)
			calibration = calibration * 3;
		printf("run_avg: %ldms, run_total: %ldms\n", run_avg, run_total);
	}
	total = invoke_c4_time() - total;

	printf("Final calibration: %ld loops chosen, total runtime %ldms\n", calibration, total);
	printf("  Run avg: %ldms  Run total: %ldms\n", run_avg, run_total);
	factor = 1;
	while ((run_avg / factor) > 1000)
		factor = factor * 10;
	cycles_per_second = ((((1000 * factor) / run_avg)) * calibration / factor);
	printf("  Cycles per second: (1000 / %ld) * %ld = ", run_avg, calibration);
	print_int_readable(cycles_per_second);
	printf(" (%ld), factor %ld\n", cycles_per_second, factor);

	// Now test it for 10 seconds, monitoring how much we're out by
	sync_test(cycles_per_second);

	free(c4_time_buf);
	return 0;
}

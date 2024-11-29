//
// C4 benchmark tool
//

#include "c4m.h"
#include "u0.c"

#define __time() c4m_time()

// Available benchmark functions
enum {
	BENCH_PI10 = 0x1,
	BENCH_PI30 = 0x2,
	BENCH_PI100 = 0x4,
	BENCH_PI1000 = 0x8,
	BENCH_PI_ALL = 0xF,
	BENCH_FACTORIAL = 0x10,
	BENCH_FACTORIAL_RECURSE = 0x20,
	BENCH_ALL = 0xFF
};

// Exit codes
enum { EXIT_OK, EXIT_HELP, EXIT_BADOPTION };

///
/// Utility functions
///

static char *readable_int_table;
static int   readable_int_max;
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
	if ((c = table[table_pos]))
		printf("%c", table[table_pos]);
}
static void print_int_readable (int n) {
	print_int_readable_using(n, " kmbt", 5);
}
static void print_time_readable (int ms) {
	print_int_readable_using(ms, "m ", 2);
}


///
/// Benchmarking functions
///

// Factorial
int benchfunc_factorial_recursive (int n) {
	if (n <= 1) return 1;
	return n * benchfunc_factorial_recursive(n - 1);
}
int benchfunc_factorial (int n) {
	int a;
	a = 1;
	while(n > 1) {
		a = a * n;
		--n;
	}
	return a;
}

int bench_factorial (int n) {
	return benchfunc_factorial_recursive(10);
}

// PI
int pow_mod(int a, int b, int m) {
	int r, aa;

	r = 1;
	aa = a;
	while (b > 0) {
		if (b % 2 == 1) {
			r = (r * aa) % m;
		}
		aa = (aa * aa) % m;
		b = b / 2;
	}
	return r;
}

// Function to compute the nth digit of pi in base 16
int nth_digit_of_pi(int n) {
	int sum, k, term1, term2, term3, term4;

	sum = 0;
	// for (int k = 0; k <= n; k++) {
	k = 0;
	while (k <= n) {
		term1 = (4 * pow_mod(16, n - k, 8 * k + 1)) / (8 * k + 1);
		term2 = (2 * pow_mod(16, n - k, 8 * k + 4)) / (8 * k + 4);
		term3 = (1 * pow_mod(16, n - k, 8 * k + 5)) / (8 * k + 5);
		term4 = (1 * pow_mod(16, n - k, 8 * k + 6)) / (8 * k + 6);
		sum = sum + (term1 - term2 - term3 - term4);
		sum = sum % 16;
		++k;
	}
	return sum;
}

int bench_pi10 () { return nth_digit_of_pi(10); }
int bench_pi30 () { return nth_digit_of_pi(30); }
int bench_pi100 () { return nth_digit_of_pi(100); }
int bench_pi1000 () { return nth_digit_of_pi(1000); }
int bench_pi_all () { bench_pi10(); bench_pi100(); bench_pi1000(); return 0; }

int bench_all () {
	benchfunc_factorial(10);
	benchfunc_factorial_recursive(10);
	bench_pi_all();
}

///
/// Options and command-line parsing
///

enum {
	PA_NONE,             // No flags
	PA_TIME,             // Time to benchmark (default parameter))
	PA_CYCLES,           // Number of benchmark repeats
	PA_BENCH             // Benchmark method
};

// TODO: long format options parsing
// -t n
// --time n     Time to measure for
int config_timeMs;
// -c n
// --repeats n  Number of repeats of measurements to run
int config_repeats;
// -b y         Benchmark to use, where y is name
// --bench
int config_bench;
// -n
// --no-delay   Do not settle the clock between benchmark repeats
int config_noDelay;
// -d
// --debug      Display debug information
int config_debug;
// -w
// --wait n     Wait n seconds before starting benchmark
int config_waitMs;
// -q
// --quiet      Quiet mode
int config_quiet;
// -Q
// --silent     Silent mode
int config_silent;
// -a argument
// --argument argument
//              Pass given argument to benchmark function
char *config_arg;
// -C
// --continuous
//              Run benchmark continuously until ctrl+c used
int config_continuous;
// -b argument
//              Benchmark to use
int *bench_function;

void show_help (char *argv0) {
	printf("%s: run a benchmark\n", argv0);
	printf("%s [-C] [-t nn] [-c nn] [-b benchmark] [-ndq] [-w nn] [-a x]\n", argv0);
	printf("       -C            Run continuously, until ctrl+c caught\n"
	       "       -t nn         Time to measure for\n"
	       "       -c nn         Number of repeats of measurement to run\n"
	       "       -b benchmark  Benchmark to use. Available options:\n"
	       "                     fac     Factorial benchmark\n"
	       "                     fac_r   Recursive factorial benchmark\n"
	       "                     pi10    10th digit of pi\n"
	       "                     pi30    30th digit of pi\n"
	       "                     pi100   100th digit of pi\n"
	       "                     pi1000  1000th digit of pi\n"
	       "                     pi      Run all pi benchmarks\n"
	       "                     all     Run all benchmarks\n"
	       "       -n            No delay, do not settle clock between benchmarks\n"
	       "       -d            Display debug information\n"
	       "       -w nn         Wait nn seconds before starting benchmark\n"
	       "       -q            Quiet mode\n"
	       "       -a x          Pass argument to benchmark (default: %s)\n", config_arg);
}

int parseArgs (int argc, char **argv) {
	int endopt;
	int _argc, *target;
	char **_argv, *arg;

	_argc = argc;
	_argv = argv;
	--_argc; ++_argv; // skip first arg
	while (_argc) {
		arg = *_argv;
		if (*arg == '-') {
			++arg;
			if (!strcmp(arg, "-help") || !strcmp(arg, "-h")) {
				return EXIT_HELP;
			}
			endopt = 0;
			while(!endopt && *arg) {
				// Single character options
				     if (*arg == 'n') config_noDelay = 1;
				else if (*arg == 'd') config_debug = 1;
				else if (*arg == 'q') config_quiet = 1;
				else if (*arg == 'Q') config_silent = config_quiet = 1; // Set both flags
				else if (*arg == 'C') config_continuous = 1;
				// Options taking an argument - numerical
				else if (*arg == 't' || *arg == 'c' || *arg == 'w') {
					     if (*arg == 't') target = &config_timeMs;
					else if (*arg == 'c') target = &config_repeats;
					else if (*arg == 'w') target = &config_waitMs;
					endopt = 1;
					--_argc; ++_argv;
					if (!_argc) {
						printf("%s: option '%s' requires an argument\n", argv[0], _argv - 1);
						return EXIT_BADOPTION;
					} else if (atoi_check(*_argv, target) != ATOI_OK) {
						printf("%s: unable to parse '%s' as a number\n", argv[0], *_argv);
						return EXIT_BADOPTION;
					}
				}
				// Options taking an argument - string
				else if (*arg == 'b') {
					--_argc; ++_argv;
					endopt = 1;
					if (!_argc) {
						printf("%s: option '-b' requires an argument\n", argv[0]);
						return EXIT_BADOPTION;
					}
					     if (!strcmp(*_argv, "fac")) config_bench = BENCH_FACTORIAL;
					else if (!strcmp(*_argv, "fac_r")) config_bench = BENCH_FACTORIAL_RECURSE;
					else if (!strcmp(*_argv, "pi10")) config_bench = BENCH_PI10;
					else if (!strcmp(*_argv, "pi30")) config_bench = BENCH_PI30;
					else if (!strcmp(*_argv, "pi100")) config_bench = BENCH_PI100;
					else if (!strcmp(*_argv, "pi1000")) config_bench = BENCH_PI1000;
					else if (!strcmp(*_argv, "pi")) config_bench = BENCH_PI_ALL;
					else if (!strcmp(*_argv, "all")) config_bench = BENCH_ALL;
					else {
						printf("%s: option '-b' requires a valid benchmark, '%s' is not valid.\n", argv[0], *_argv);
						return EXIT_BADOPTION;
					}
				} else if (*arg == 'a') {
					--_argc; ++_argv;
					endopt = 1;
					if (!_argc) {
						printf("%s: option '-a' requires an argument\n", argv[0]);
						return EXIT_BADOPTION;
					}
					config_arg = arg;
				} else {
					printf("%s: option '%c' not recognised\n", argv[0], *arg);
					return EXIT_BADOPTION;
				}
				++arg;
			}
		} else {
			printf("%s: invalid option '%s'\n", argv[0], arg);
			show_help(argv[0]);
			return EXIT_HELP;
		}
		--_argc; ++_argv;
	}
	return EXIT_OK;
}

///
/// Signal handling (for continuous mode)
///

int stop_continuous;
void sighandler_SIGINT () {
	if (!stop_continuous)
		printf("bench: caught SIGINT, stopping after this cycle...\n");
	++stop_continuous;
}
void sighandler_SIGTERM () {
	if (!stop_continuous)
		printf("bench: caught SIGTERM, stopping after this cycle...\n");
	++stop_continuous;
}

///
/// Timing functions
///

// Get the time, but ensure that it has changed at least once.
// This expensive functions allows more accurate time measurement under plain c4.
int time_settle () {
	int t, last_t;

	last_t = __time();

	if (config_noDelay)
		return last_t;

	if (config_debug) printf("time_settle(){\n");
	while((t = __time()) - last_t < 100)
		sleep(100 + (pid() * 20)); // do nothing
	if (config_debug) printf("time_settle()}\n");
	return t;
}

///
/// Benchmark and timing functions
///
int benchmark_single () {
	int now, cycles, targetTime;
	now = time_settle();
	cycles = 0;
	targetTime = now + config_timeMs;

	while(now < targetTime) {
#define bench_function() ((int (*)())bench_function)()
		bench_function();
#undef bench_function
		++cycles;
		now = __time();
	}
	return cycles;
}

int totalBenchCycles, totalCpuCycles, totalBenchTime;
void benchmark_repeat (int repeats) {
	int cycles, last;
	while(repeats--) {
		cycles = __c4_cycles();
		last   = __time();
		totalBenchCycles = totalBenchCycles + benchmark_single();
		totalCpuCycles = totalCpuCycles + __c4_cycles() - cycles;
		totalBenchTime = totalBenchTime + (__time() - last);
	}
}

void benchmark_report () {
	int timeStart, timeStop, keep_going;

	keep_going = 1; // run at least one cycle
	while (keep_going) {
		// Benchmark
		totalBenchCycles = totalCpuCycles = totalBenchTime = 0;
		timeStart = __time();
		if (config_debug) printf("Benchmark starting...\n");
		benchmark_repeat(config_repeats);
		timeStop = __time();
		if (!config_silent) {
			printf( "Benchmark complete: ");
			// We need totalBenchCycles to be large enough to be divided nicely
			print_int_readable((totalBenchCycles * 5000) / config_repeats / totalBenchTime);
			printf(" score\n  ");
			print_int_readable(totalBenchCycles);
			printf(" cycles over ");
			print_time_readable(totalBenchTime);
			printf("s (%dms real time)\n", timeStop - timeStart);
			if (config_debug) {
				printf("Total CPU cycles: ");
				print_int_readable(totalCpuCycles);
				printf("\n");
			}
		}
		
		// Update keep_going flag and settle if we are to keep going
		if ((keep_going = config_continuous && !stop_continuous))
			sleep(config_waitMs);
	}
}

///
/// Main
///

int main (int argc, char **argv) {
	int i;

	readable_int_table = " kmbt";
	readable_int_max   = 5;

	// Set defaults
	config_timeMs  = 1000;
	config_repeats = 2;
	config_bench   = BENCH_FACTORIAL; //BENCH_PI10; //BENCH_ALL; //BENCH_FACTORIAL;
	config_noDelay = 0;
	config_debug   = 0;
	config_waitMs  = 2000;
	config_quiet   = 0;
	config_silent  = 0;
	config_arg     = "10";
	config_continuous = 0;
	stop_continuous= 0;

	if ((i = parseArgs(argc, argv)) != EXIT_OK) {
		show_help(argv[0]);
		return i;
	}

	if (config_continuous) {
		printf("bench: continuous option (-C) enabled, use CTRL+C to finish.\n");
		if (platform_is_c4_plain())
			printf("bench: warning, running on C4 without CTRL+C support\n");
		// Only install these handlers in continuous mode, without it just use normal exit handlers.
		signal(SIGINT, (int *)&sighandler_SIGINT);
		signal(SIGTERM, (int *)&sighandler_SIGTERM);
	}

	if (config_bench == BENCH_FACTORIAL)   bench_function = (int *)&bench_factorial;
	else if (config_bench == BENCH_PI10)   bench_function = (int *)&bench_pi10;
	else if (config_bench == BENCH_PI30)   bench_function = (int *)&bench_pi30;
	else if (config_bench == BENCH_PI100)  bench_function = (int *)&bench_pi100;
	else if (config_bench == BENCH_PI1000) bench_function = (int *)&bench_pi1000;
	else if (config_bench == BENCH_PI_ALL) bench_function = (int *)&bench_pi_all;
	else if (config_bench == BENCH_ALL)    bench_function = (int *)&bench_all;
	else {
		printf("Invalid benchmark function\n");
		return 1;
	}

	if (config_waitMs) {
		//if (config_quiet)
		//	printf("Waiting for %dms before starting benchmark...\n", config_waitMs);
		sleep(config_waitMs);
	}
	benchmark_report();

	return 0;
}

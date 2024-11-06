//
// Run several instances of 'bench' in their own c4(m) processes.
//
// Specify -h for full commandline options.
//

#include "u0.c"

enum { MAX_ARGV = 16 };
enum {
	ONLY_C4  = 0x1,
	ONLY_C4M = 0x2,
	ONLY_BENCH = 0x4, // Only run bench directly
	ONLY_LOADC4R = 0x8,
	ONLY_DIRECT = 0x10, // No longer valid
	WITH_VERBOSE = 0x20,
	WITH_VFS = 0x40,
	WITH_TESTS = 0x80,
	USE_QUIET = 0x100,
	// For c4m, required on some versions of linux
	// Changes how /proc/uptime is interpreted
	USE_ALTMODE = 0x200,
	// Spawn benchtop instead of bench (less options)
	USE_BENCHTOP = 0x400,
};
enum {
	EXIT_OK = 0,
	EXIT_HELP = 1,
	EXIT_BADALLOC = 2,
	EXIT_BADOPTION = 3
};

int _argc;
char **_argv;
int *pids, pid_counter;
int pid_top;

void psh (char *str) {
	if (_argc == MAX_ARGV) {
		printf("innerbench: trying to push too many arguments!\n");
		return;
	}
	_argv[_argc++] = str;
}

void print_args () {
	int    c, i;
	char **v;
	c = _argc;
	v = _argv;
	i = 0;
	while(c) {
		if (i > 0) printf(" ");
		printf("%s", _argv[i]);
		--c;
		++i;
	}
}

int sent_shutdown;
void gracefully_shutdown () {
	int i, running, pid;
	if (sent_shutdown) {
		printf("innerbench: shutdown already sent! terminating early\n");
		exit(-1);
	}
	sent_shutdown = 1;
	printf("innerbench: terminating benchmark processes...\n");
	running = 1;
	while (running > 0) {
		running = 0;
		i = 0;
		while (i < pid_counter) {
			if ((pid = pids[i])) {
				if (pid < 0) pid = -pid;
				if (kern_task_running(pid)) {
					if (pid > 0) {
						kill(pid, SIGTERM);
					}
					++running;
				}
			}
			++i;
		}
		sleep(1000);
	}
	printf("innerbench: benchmark processes shutdown\n");
	if (pid_top) {
		printf("innerbench: stopping top\n");
		kill(pid_top, SIGTERM);
	}
	printf("innerbench: gracefully shutdown\n");
}

// Signal handlers
void sighandler_SIGTERM () {
	printf("innerbench: caught SIGTERM\n");
	gracefully_shutdown();
	exit(0);
}
void sighandler_SIGINT () {
	printf("innerbench: caught SIGINT\n");
	gracefully_shutdown();
	exit(0);
}


void show_help (char *invoke) {
	printf("%s: run a set of inner benchmarks in their own c4(m) instances.\n", invoke);
	printf("%s [-n x] [-aTBDqcmlvVt] [-b args]\n", invoke);
	printf("      -n x    Start x (number) inner benchmarks\n"
	       "              This option can be given in combination with others but must be\n"
	       "              followed by a space and number.\n"
	       "      -a      Pass `-a` to `c4m`, to use alternate time mode\n"
	       "      -T      Disable running `top` during execution\n"
	       "      -B      Enable direct bench execution (ignores below options except -b)\n"
	       "      -D      Launch `benchtop` instead of `bench`\n"
	       "      -q      Enable quiet mode for benchmark\n"
	       "      -c      Enable C4 mode (ignored if -m given)\n"
	       "      -m      Enable C4M mode (default if -c not given)\n"
	       "      -l      Use load-c4r by itself (ignored if -d given)\n"
	       //"      -d      Use load-c4r + c4ke.c (default)\n"
	       "      -v      Enable verbose mode\n"
	       "      -V      Enable VFS process\n"
	       "      -t      Enable test processes\n"
	       "\n"
	       "      -b args Pass args to bench\n");
}

int main (int argc, char **argv) {
	int   i, inner_benches, lastpid, endopt, atoi_r;
	int   mode, notop, quiet;
	char *arg, *title, *bench_arg, *arg_start, ch;
	int   running, startTime;

	// Set kernel focus to this task so that ctrl+c kills it
	c4ke_set_focus(pid());

	sent_shutdown = 0;

	signal(SIGTERM, (int *)&sighandler_SIGTERM);
	signal(SIGINT, (int *)&sighandler_SIGINT);

	inner_benches = 2; // number of bench processes
	mode = ONLY_C4M | ONLY_DIRECT;
	notop = 0;
	quiet = 0;
	bench_arg = 0;

	// Parse args
	_argc = argc;
	_argv = argv;
	--_argc; ++_argv; // skip first arg
	while(_argc) {
		arg = *_argv;
		if (*arg == '-') {
			++arg;
			if (!strcmp(arg, "-help") || !strcmp(arg, "-h")) {
				show_help(argv[0]);
				return EXIT_HELP;
			}
			// Iterate through
			endopt = 0;
			while(!endopt && *arg) {
				if (*arg == 'c') mode = (mode & ~ONLY_C4M) | ONLY_C4;
				else if (*arg == 'q') mode = mode | USE_QUIET;
				else if (*arg == 'm') mode = (mode & ~ONLY_C4) | ONLY_C4M;
				else if (*arg == 'l') mode = mode | ONLY_LOADC4R;
				else if (*arg == 'd') mode = mode | ONLY_DIRECT;
				else if (*arg == 'v') mode = mode | WITH_VERBOSE;
				else if (*arg == 'V') mode = mode | WITH_VFS;
				else if (*arg == 't') mode = mode | WITH_TESTS;
				else if (*arg == 'a') mode = mode | USE_ALTMODE;
				else if (*arg == 'B') mode = ONLY_BENCH;
				else if (*arg == 'D') mode = mode | USE_BENCHTOP;
				else if (*arg == 'T') notop = 1;
				else if (*arg == 'b') {
					endopt = 1;
					--_argc; ++_argv;
					if (!_argc) {
						printf("%s: option '-b' requires an argument\n", argv[0]);
						return EXIT_BADOPTION;
					} else {
						if (bench_arg) {
							printf("%s: warning, option '-b' specified more than once. Overwriting with last value given.\n", argv[0]);
						}
						bench_arg = *_argv;
					}
				}
				else if (*arg == 'n') {
					endopt = 1;
					// grab number from next argv
					--_argc; ++_argv;
					if (!_argc) {
						printf("%s: option '-n' requires an argument\n", argv[0]);
						return EXIT_BADOPTION;
					} else {
						if ((atoi_r = atoi_check(*_argv, &inner_benches)) != ATOI_OK) {
							printf("%s: unable to parse '%s' as a number\n", argv[0], *_argv);
							return EXIT_BADOPTION;
						}
					}
				} else {
					printf("%s: invalid option '%s'\n", argv[0], arg - 1);
					show_help(argv[0]);
					return EXIT_HELP;
				}
				++arg;
			}
		} else {
			printf("%s: unknown option '%s'\n", argv[0], arg);
			show_help(argv[0]);
			return EXIT_HELP;
		}
		--_argc; ++_argv;
	}

	if (!(mode & USE_QUIET))
		printf("%s v0.2 starting...\n", argv[0]);

	if (!(_argv = malloc((i = sizeof(char **) * MAX_ARGV)))) {
		printf("%s: unable to allocate %d bytes for argv\n", argv[0], i);
		return EXIT_BADALLOC;
	}
	memset(_argv, 0, i);
	if (!(pids = malloc((i = sizeof(int) * inner_benches)))) {
		free(_argv);
		printf("%s: unable to allocate %d bytes for pids\n", argv[0], i);
		return EXIT_BADALLOC;
	}
	memset(pids, 0, i);
	pid_counter = 0;

	_argc = 2;
	_argv[0] = "top";
	_argv[1] = "-b"; // background mode
	pid_top = 0;
	if (!notop) {
		pid_top = kern_user_start_c4r(_argc, _argv, "top", PRIV_USER);
		if (mode & WITH_VERBOSE)
			printf("%s: top started in the background, pid %d...\n", argv[0], pid_top);
	}

	// Calculate the invocation
	if (!(mode & USE_QUIET)) {
		printf("%s: Spawning %d inner benchmarks with mode 0x%x\n", argv[0], inner_benches, mode);
	}
	lastpid = 0;
	title = (mode & ONLY_BENCH) ? "bench" : "inner-kernel";
	_argc = 0;
	if (!(mode & ONLY_BENCH)) {
		if (mode & ONLY_C4) {
			psh("c4");
			psh("c4m.c");
			if (mode & USE_ALTMODE)
				psh("-a");
		} else if (mode & ONLY_C4M) {
			psh("c4m");
			if (mode & USE_ALTMODE)
				psh("-a");
		}
		// Can cause a segfault, disabled
		//if (mode & WITH_VERBOSE)
		//	psh("-v");
		psh("load-c4r.c");
		if (mode & ONLY_LOADC4R) {
			psh("--");
			//if (mode & WITH_VERBOSE)
			//	psh("-v");
			psh("c4ke");
		} else if (mode & ONLY_DIRECT) {
			psh("c4ke.c");
			psh("--");
		}
		// Verbosity
		psh("-v");
		if (mode & WITH_VERBOSE)
			psh("50"); // medium
		else
			psh("0");
		// Other flags
		if (mode & WITH_VFS)
			psh("-V");
		if (mode & WITH_TESTS)
			psh("-t");
	}

	// Bench command
	if (mode & USE_BENCHTOP)
		psh("benchtop");
	else {
		psh("bench");
		if (mode & USE_QUIET)
			psh("-q");
		if (mode & WITH_VERBOSE)
			psh("-d");
		if (bench_arg) {
			// Push each given argument
			arg = arg_start = bench_arg;
			while((ch = *arg)) {
				if (ch == ' ') {
					// Change to nul and push it
					*arg = 0;
					psh(arg_start);
					// Update pointers
					arg = arg_start = arg + 1;
				} else {
					++arg;
				}
			}
			// if anything remaining...
			if (arg != arg_start) {
				psh(arg_start);
			}
		}
	}
	if (!(mode & USE_QUIET)) {
		// Print invocation
		printf("%s: invoking '", argv[0]);
		print_args();
		printf("'\n");
	}

	// start inner benchmark processes
	while (inner_benches--) {
		lastpid = kern_user_start_c4r(_argc, _argv, title, PRIV_USER);
		// printf("%s: pid %d started\n", argv[0], lastpid);
		pids[pid_counter++] = lastpid;
	}

	// Record start time
	startTime = __time();

	// wait on the last pid
	if (lastpid) {
		await_pid(lastpid);
	}

	printf("%s: First benchmark complete in %ldms\n", argv[0], __time() - startTime);

	// Check pids
	running = 1;
	if (!(mode & USE_QUIET))
		printf("%s: waiting for benchmarks to finish...\n", argv[0]);
	while (running > 0) {
		i = 0;
		running = 0;
		while (i < pid_counter) {
			if (pids[i]) {
				if (kern_task_running(pids[i])) {
					// kill(pids[i], SIGTERM);
					// pids[i] = 0;
					++running;
				}
			}
			++i;
		}
		if (running > 0)
			sleep(1000);
	}


	printf("%s: benchmark processes finished in %ldms\n", argv[0], __time() - startTime);

	sleep(2000);

	// Cleanup top
	if (pid_top) {
		if (mode & WITH_VERBOSE)
			printf("%s: stopping top\n", argv[0]);
		kill(pid_top, SIGTERM);
		// Wait for top to finish
		await_pid(pid_top);
	}

	free(_argv);
	free(pids);

	if (!(mode & USE_QUIET))
		printf("%s complete\n", argv[0]);
	return EXIT_OK;
}

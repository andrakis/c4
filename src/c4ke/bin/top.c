//
// top.c: A simple topography viewer to show the state of running tasks.
//
#include "c4.h"
#include "c4m.h"

#define PS_NOMAIN 1
#include "ps.c"

// Until kern_tasks_running() implemented
#if C4
int kern_tasks_running() { return ps_count_running; }
#endif

// Globals
int run;

// Signal handlers
void sighandler_SIGTERM () {
	// printf("top: exiting nicely\n");
	ps_uninit();
	exit(0);
}

enum { EXIT_OK, EXIT_HELP, EXIT_BADOPTION };
void show_help (char *invoke) {
	printf("%s: display C4KE processes\n", invoke);
	printf("%s [-b] [-d nn] [-n nn]\n", invoke);
	printf("      -d nn  Set refresh delay to nn milliseconds\n");
	printf("      -b     Background mode, do not set focus. Requires -n or top will run forever.\n");
	printf("      -n nn  Show the listing nn times before exiting\n");
}

enum {
	MODE_BACKGROUND,
	MODE_FOREGROUND
};

// Main
int main (int argc, char **argv) {
	int i;
	int run;
	int sleep_ms, cycles, mode;
	int _argc, endopt, *target;
	char **_argv, *arg;

	if ((i = ps_init())) {
		// inform parent something went wrong
		kill(parent(), SIGUSR2);
		return i;
	}

	cycles = -1;
	sleep_ms = 1000;
	endopt = 0;
	target = 0;
	mode   = MODE_FOREGROUND;
	if (c4_plain)
		cycles = 1;

	// TODO: move out of main, don't exit immediately there, send
	//       SIGUSR2 on failure.
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
			} else {
				// Iterate through
				endopt = 0;
				while(!endopt && *arg) {
					if (*arg == 'b') { mode = MODE_BACKGROUND; cycles = -1; }
					else if (*arg == 'n') {
						endopt = 1;
						// grab number from next argv
						--_argc; ++_argv;
						if (!_argc) {
							printf("%s: option '-n' requires an argument\n", argv[0]);
							return EXIT_BADOPTION;
						} else {
							if (atoi_check(*_argv, &cycles) != ATOI_OK) {
								printf("%s: unable to parse '%s' as a number\n", argv[0], *_argv);
								return EXIT_BADOPTION;
							}
						}
					} else if (*arg == 'd') {
						endopt = 1;
						// grab number from next argv
						--_argc; ++_argv;
						if (!_argc) {
							printf("%s: option '-d' requires an argument\n", argv[0]);
							return EXIT_BADOPTION;
						} else {
							if (atoi_check(*_argv, &sleep_ms) != ATOI_OK) {
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
			}
		}
		--_argc; ++_argv;
	}

	// Inform parent we're starting successfully (for use as service testing)
	kill(parent(), SIGUSR1);
	signal(SIGTERM, (int *)&sighandler_SIGTERM);

	run = 1;
	if (c4_plain) {
		printf("c4top: not an interactive terminal, batch mode enabled\n");
	} else {
		printf("c4top: use ctrl+c to exit\n");
	}
	if (mode == MODE_FOREGROUND)
		c4ke_set_focus(pid());

	while(run) {
		ps();
		printf("----------\n");
		if (cycles > 0) {
			if (--cycles == 0)
				run = 0;
		}
		if (run)
			sleep(sleep_ms);
	}

	ps_uninit();
	return 0;
}

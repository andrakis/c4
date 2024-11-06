// C4SH builtin commands
//
// This file contains the builtin commands for c4sh. They are added to
// c4sh's builtins via a constructor.
// One of the reason for these commands being builtin is that they can
// be used even when at the process limit, allowing for some amount of
// recovery.
//
// For scripting related commands, see c4sh_scripting.c
//
// TODO: these copy many bash builtins usage wording verbatim.
//
// Current builtins:
//  - fg
//  - help
//  - kill
//  - ps
//  - toggle
//
// Planned:

#include "c4sh.c"

///
// fg
///

void fg_usage (char *argv0) { }
int  fg_run   (int argc, char **argv) {
	int i;

	if (argc > 1) {
		if (atoi_check(argv[1], &i) != ATOI_OK) {
			printf("c4sh: unable to understand '%s'\n", argv[1]);
		} else {
			foreground_process(i);
		}
	} else {
		if (last_pid == 0) {
			printf("c4sh: no backgrounded task started yet, give a pid number\n");
		} else {
			foreground_process(last_pid);
		}
	}
	return CR_FAIL;
}

///
// help
//
// Display builtin commands or get usage on a particular command.
///

void help_usage (char *argv0) {
	printf("Displays help on builtin commands, or the specified command.\n");
	printf("Example: help\n");
	printf("         help toggle\n");
}
int  help_run   (int argc, char **argv) {
	int id, *builtin, *usage;
	if (argc == 1)
		printf("Commands:\n");

	id = 0;
	builtin = builtins;
	while(id < builtins_used) {
		if (argc <= 1 || !strcmp((char *)builtin[CMD_NAME], argv[1])) {
			printf("		%s %s", (char *)builtin[CMD_NAME], (char *)builtin[CMD_USAGE_SHORT]);
			printf("		%s\n", (char *)builtin[CMD_DESC]);
			if (argc > 1) {
				builtin_usage(builtin, argv[0]);
			}
		}
		++id;
		builtin = builtin + CMD__Sz;
	}

	return CR_OK;
}

///
// kdbg
///

void kdbg_usage (char *argv0) { }
int  kdbg_run   (int argc, char **argv) {
	debug_kernelstate();
	return CR_OK;
}

///
// kill
//
// Send a signal to a job.
///

void kill_usage (char *argv0) {
	printf("%s: %s [-s sigspec | -n signum | -sigspec] pid | jobspec ... or kill -l [sigspec]\n",
	        argv0, argv0);
	printf(
"     Send a signal to a job.\n"
"\n"
"     Send the processes identified by PID or JOBSPEC the signal named by\n"
"     SIGSPEC or SIGNUM.  If neither SIGSPEC nor SIGNUM is present, then\n"
"     SIGTERM is assumed.\n"
"\n"
"     Options:\n"
//"       -s sig    SIG is a signal name\n" // TODO: signals by name
//"       -n sig    SIG is a signal number\n"
"       -l        list the signal names; if arguments follow `-l' they are\n"
"                 assumed to be signal numbers for which names should be listed\n"
"       -L        synonym for -l\n"
"\n"
//"     Kill is a shell builtin for two reasons: it allows job IDs to be used\n"
//"     instead of process IDs, and allows processes to be killed if the limit\n"
//"     on processes that you can create is reached.\n"
//"\n"
"     Exit Status:\n"
"     Returns success unless an invalid option is given or an error occurs.\n");
}

char **signal_names;
int setup_signal_names () {
	int t;

	if (!(signal_names = malloc((t = sizeof(char *) * (SIGMAX + 1)))))
		return 1;
	memset(signal_names, 0, t);

	signal_names[SIGHUP] = "SIGHUP";
	signal_names[SIGINT] = "SIGINT";
	signal_names[SIGQUIT] = "SIGQUIT";
	signal_names[SIGILL] = "SIGILL";
	signal_names[SIGTRAP] = "SIGTRAP";
	signal_names[SIGABRT] = "SIGABRT";
	signal_names[SIGBUS] = "SIGBUS";
	signal_names[SIGFPE] = "SIGFPE";
	signal_names[SIGKILL] = "SIGKILL";
	signal_names[SIGUSR1] = "SIGUSR1";
	signal_names[SIGSEGV] = "SIGSEGV";
	signal_names[SIGUSR2] = "SIGUSR2";
	signal_names[SIGPIPE] = "SIGPIPE";
	signal_names[SIGALRM] = "SIGALRM";
	signal_names[SIGTERM] = "SIGTERM";
	signal_names[SIGSTKFLT] = "SIGSTKFLT";
	signal_names[SIGCHLD] = "SIGCHLD";
	signal_names[SIGCONT] = "SIGCONT";
	signal_names[SIGSTOP] = "SIGSTOP";
	signal_names[SIGTSTP] = "SIGTSTP";
	signal_names[SIGTTIN] = "SIGTTIN";
	signal_names[SIGTTOU] = "SIGTTOU";
	signal_names[SIGURG] = "SIGURG";
	signal_names[SIGXCPU] = "SIGXCPU";
	signal_names[SIGXFSZ] = "SIGXFSZ";
	signal_names[SIGVTALRM] = "SIGVTALRM";
	signal_names[SIGPROF] = "SIGPROF";
	signal_names[SIGWINCH] = "SIGWINCH";
	signal_names[SIGIO] = "SIGIO";
	signal_names[SIGPWR] = "SIGPWR";
	signal_names[SIGSYS] = "SIGSYS";
	signal_names[SIGRTMIN] = "SIGRTMIN";
	signal_names[SIGRTMAX] = "SIGRTMAX";
	signal_names[SIGMAX] = "SIGMAX";

	return 0;
}

int signal_lookup (char *name) {
	int sig, base, n;
	char operator;

	sig = 1;
	while (sig <= SIGRTMIN) {
		if (!strcmp(name, signal_names[sig]))
			return sig;
		++sig;
	}

	base = 0;
	// SIGRT[MIN,MAX][+,-]n
	if (!strcmp("SIGRTMIN", name)) base = SIGRTMIN;
	else if (!strcmp("SIGRTMAX", name)) base = SIGRTMAX;
	else {
		printf("invalid signal name '%s'\n", name);
		return 0;
	}

	// Find +,-
	while (*name && *name != '+' && *name != '-') ++name;
	if (*name) {
		operator = *name;
		++name;
		// Find number
		while (*name && *name == ' ') ++name;
		if (atoi_check(name, &n) != ATOI_OK) {
			printf("invalid number in signal name\n");
			return 0;
		}
		return base + n;
	}

	printf("unknown name section\n");
	return 0;
}

void print_signals () {
	int sig, items_printed;

	sig = 1;
	items_printed = 0;
	while (sig <= SIGMAX) {
		printf("%2d) ", sig);
		if (signal_names[sig]) printf("%s  ", signal_names[sig]);
		else if (sig > SIGRTMIN && sig < SIGRTMAX) printf("SIGRTMIN+%2d  ", sig - SIGRTMIN);
		else if (sig == SIGRTMAX) printf("SIGRTMAX");
		else printf("(unknown)  ");
		printf("%c%c", TAB, TAB); // TODO: tab not printing properly
		if (++items_printed == 5) { printf("\n"); items_printed = 0; }
		++sig;
	}
	printf("\n");
}

int kill_run (int argc, char **argv) {
	char *input;
	int   pid, sig;

	if (argc == 1) {
		kill_usage(argv[0]);
		return CR_OK;
	}

	input = argv[1];
	if (*input == '-' && (*(input + 1) == 'l' || *(input + 1) == 'L')) {
		print_signals();
		return CR_OK;
	}

	if (atoi_check(input, &pid) != ATOI_OK) {
		printf("c4sh: unable to understand '%s'\n", input);
		return CR_OK;
	}

	sig = SIGTERM; // default
	if (argc == 3) {
		if (atoi_check(argv[2], &sig) != ATOI_OK) {
			// check signal names
			sig = signal_lookup(argv[2]);
			if (!sig) {
				printf("c4sh: unable to understand '%s' as a sig number or name, try -l\n",
					   argv[2]);
			}
		}
	}

	if (sig) {
		kill(pid, sig);
		printf("c4sh: sent signal %d to pid %d\n", sig, pid);
		// sleep a moment
		sleep(1);
	}

	return CR_OK;
}

///
// ps
///

void ps_usage (char *argv0) { }
int  ps_run   (int argc, char **argv) {
	ps();
	return CR_OK;
}

///
// toggle
///

void toggle_usage (char *argv0) {
	printf("Available flags to toggle:\n");
	printf("	psf              Process listing in foreground (primitive 'top')\n");
}
int  toggle_run   (int argc, char **argv) {
	if (argc == 1) {
		toggle_usage(argv[0]);
	} else if (!strcmp(argv[1], "psf")) {
		enable_ps_on_wait = !enable_ps_on_wait;
		printf("c4sh: ps in foreground: %s\n", enable_ps_on_wait ? "on" : "off");
	} else {
		toggle_usage(argv[0]);
		printf("toggle: don't know what '%s' is\n", argv[1]);
	}

	return CR_OK;
}


///
// Early entry point: constructor
// We must get c4sh to a usable state before we attempt to add builtin commands.
///
static int __attribute__((constructor)) c4sh_builtins_init (int *c4r) {
	// printf("c4sh_builtin_init()\n");
	if (c4sh_builtin_init() == CR_FAIL) {
		printf("c4sh: c4sh_builtin_init() failed\n");
		return 1;
	}

	if (setup_signal_names() != 0)
		printf("c4sh: warning, failed to setup signal names.\n");

	// Now add each of the builtins
	if (CR_FAIL == c4sh_builtin_register(
		"fg",                       // Command name
		"Foreground a process",     // Short description
		"[pid]",                    // Short usage
		(int *)&fg_usage,           // Callback for help fg
		(int *)&fg_run))            // Callback to run the command
		printf("c4sh: failed to register builtin 'fg'\n");
	if (CR_FAIL == c4sh_builtin_register(
		"help",                     // Command name
		// Short description
		"Display this help screen or usage for a particular command",
		"[command]",                // Short usage
		(int *)&help_usage,         // Callback for help help
		(int *)&help_run))          // Callback to run the command
		printf("c4sh: failed to register builtin 'help'\n");
	if (CR_FAIL == c4sh_builtin_register(
		"kdbg",                     // Command name
		"Print kernel information", // Short description
		"     ",                    // Short usage
		(int *)&kdbg_usage,         // Callback for help kdbg
		(int *)&kdbg_run))          // Callback to run the command
		printf("c4sh: failed to register builtin 'kdbg'\n");
	if (CR_FAIL == c4sh_builtin_register(
		"kill",                     // Command name
		"Send a signal to a job",   // Short description
		"[-L] [pid]",               // Short usage
		(int *)&kill_usage,         // Callback for help kill
		(int *)&kill_run))          // Callback to run the command
		printf("c4sh: failed to register builtin 'kill'\n");
	if (CR_FAIL == c4sh_builtin_register(
		"ps",                       // Command name
		"Print process listing",    // Short description
		"     ",                    // Short usage
		(int *)&ps_usage,           // Callback for help ps
		(int *)&ps_run))            // Callback to run the command
		printf("c4sh: failed to register builtin 'ps'\n");
	if (CR_FAIL == c4sh_builtin_register(
		"toggle",                   // Command name
		"Toggle a C4SH setting",    // Short description
		"[psf]",                    // Short usage
		(int *)&toggle_usage,       // Callback for help toggle
		(int *)&toggle_run))        // Callback to run the command
		printf("c4sh: failed to register builtin 'toggle'\n");

	return 0;
}

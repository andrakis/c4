// C4SH scripting builtin commands
//
// This file contains the scripting builtin commands for c4sh. They are added to
// c4sh's builtins via a constructor.
// Many of these functions deal with the internals of c4sh's state.
//
// Current builtins:
//  - echo
//  - exit
//  - set
//
// Planned:
//  - cd

#include "c4sh_builtins.c"

///
// echo
///

void echo_usage (char *argv0) {
	printf("Write arguments to the standard output.\n"
	       "\n"
	       "[-n]       Suppress newline\n");
}
int  echo_run   (int argc, char **argv) {
	int i, suppress_newline;

	suppress_newline = 0;
	if (argc > 1) {
		i = 1;
		if (!strcmp("-n", argv[1])) {
			suppress_newline = 1;
			++i;
		}

		while(i < argc) {
			if (i > 1) printf(" ");
			printf("%s", argv[i]);
			++i;
		}
	}
	if (!suppress_newline) printf("\n");

	return CR_OK;
}

///
// exit
///

void exit_usage (char *argv0) { }
int  exit_run   (int argc, char **argv) {
	int code;

	shell_mode = SH_EXIT;
	if (argc > 1) {
		if (atoi_check(argv[1], &code) != ATOI_OK) {
			printf("c4sh: exit code '%s' does not appear to be a number\n");
			exit_code = 255;
		} else {
			exit_code = code;
		}
	}
	return CR_OK;
}

///
// set
///

void set_usage (char *argv0) { }

void dump_set () {
	char *e;
	int   len;
	e = environment;
	while(e < env_max) {
		if(*e) {
			len = strlen(e);
			printf("%c%.*s\n", TAB, len, e);
			e = e + len + 1;
		} else ++e;
	}
}

int  set_run   (int argc, char **argv) {
	char *v;

	if(argc == 1) {
		dump_set();
	} else {
		if (argc == 2) {
			if (!(v = env_find(argv[1]))) {
				printf("c4sh: variable '%s' is not set to a value\n", argv[1]);
			} else {
				printf("	%s=%s\n", argv[1], v);
			}
		} else if (argc == 3) {
			env_set(argv[1], argv[2]);
		} else {
			printf("c4sh: set: unsure what to do, be simple\n");
		}
	}
	return CR_FAIL;
}

///
// Early entry point: constructor
// We must get c4sh to a usable state before we attempt to add builtin commands.
///
static int __attribute__((constructor)) c4sh_scripting_init (int *c4r) {
	// printf("c4sh_scripting_init()\n");
	if (c4sh_builtin_init() == CR_FAIL) {
		printf("c4sh: c4sh_scripting_init() failed\n");
		return 1;
	}

	// Now add each of the builtins
	if (CR_FAIL == c4sh_builtin_register(
		"echo",                     // Command name
		"Echo a line",              // Short description
		" ",                        // Short usage
		(int *)&echo_usage,         // Callback for help echo
		(int *)&echo_run))          // Callback to run the command
		printf("c4sh: failed to register builtin 'echo'\n");
	if (CR_FAIL == c4sh_builtin_register(
		"exit",                     // Command name
		"Exit the current script",  // Short description
		"[exit code]",              // Short usage
		(int *)&exit_usage,         // Callback for help exit
		(int *)&exit_run))          // Callback to run the command
		printf("c4sh: failed to register builtin 'exit'\n");
	if (CR_FAIL == c4sh_builtin_register(
		"set",                       // Command name
		// Short description
		"set or display environment variables",
		"[VAR1=VALUE1] [VAR2...]",   // Short usage
		(int *)&set_usage,           // Callback for help set
		(int *)&set_run))            // Callback to run the command
		printf("c4sh: failed to register builtin 'set'\n");

	return 0;
}


//
// C4SH - The C4 SHell for C4KE
//
// A shell with more advanced features than the emergency shell, such as:
//  - builtin commands: help, fg, kill, set
//  - detailed usage for builtin commands via help
//  - functions
//  - running scripts
//
// Compilation:
//  ./c4r -o c4sh.c4r u0.c c4sh.c c4sh_builtins.c
//  or let the Makefile do it for you as part of the run command:
//  make run
//
// Usage:
//  - Type "help" for a command listing.
//  - Any .c4r file can be run, and the .c4r extension does not need to be passed.
//  - You can start a task in the background using & like in bash:
//    top &
//    - kill not implemented
//    - you can foreground the most recently executed background task by using:
//      fg
//  - Any task running in the foreground causes c4sh to wait, saving CPU cycles.
//  - Cursor keys do not work on the readline we have available. However, pressing
//    up and enter will run the last command again.
//  - #comments are ignored until end of line
//  - Double or single quotes can be used:
//    innerbench -Bn 10 -b "-b pi10"
//    - There is no text substitution in quotes, they merely group arguments together.
//    - Both quotes do the same thing, but you can "'nest quotes' for example".
//    - Quotes cannot be \"escaped\".

#include "c4.h"
#include "c4m.h"

#include "u0.c"
#define PS_NOMAIN 1
#include "ps.c"

// Streams
enum { STDIN, STDOUT, STDERR };

// Default values
enum { BUFFER_SZ = 1024, ENV_SZ = 31024,
       PS_INTERVAL_DEFAULT = 1000, PS_ON_WAIT_DEFAULT = 0
};
enum {
	SLEEP_TIME = 100
};

enum { // Shell modes
	SH_EXIT,
	SH_LOOP,
	SH_PROC
};

enum {
	CR_FAIL,        // Generic failure
	CR_OK,          // No error
};

// structure: shell command dictionary
// Builtin commands structure.
enum {
	CMD_NAME,        // char *, 0 if not set
	CMD_NAMELEN,     // int
	CMD_DESC,        // char *, description shown in help
	CMD_USAGE_SHORT, // char *, usage shown in help
	CMD_USAGE,       // int *, pointer to function: void ()
	CMD_RUN,         // int *, pointer to function: int (int argc, char **argv) -> CR_* enum
	CMD__Sz          // size of struct
};

//
// Globals
//
static char *user_input, *user_prompt;
static int   user_input_len;
static char *prev_input;
static int   prev_input_len;
static int   shell_mode, exit_code;
static char *version;
static char *environment, *env_max;
static int   last_pid;

static int   enable_ps_on_wait;
static int   ps_interval;

enum { BUILTIN_LIMIT = 32 };
static int  *builtins;      // builtin commands
static int   builtins_size; // and length of the vector
static int   builtins_used; // and current usage

enum { SHELL_TEXT, SHELL_QUOTE, SHELL_DBL_QUOTE, SHELL_END, SHELL_BACKGROUND };
enum { SHELL_DATA_SIZE = 4096 }; // Overkill?
enum { SHELLF_NONE, SHELLF_BACKGROUND = 1 };
static char **c4_argv;
static int    c4_argc;
static char *shell_tk;
static int   shell_tk_type; // SHELL_*
static char *shell_text;

//
// Utility functions
//
static char *strcpycat (char *source, char *append) {
	int length, slen, alen;
	char *buffer, *s, *d;

	// Count length
	length = slen = alen = 0;
	s = source; while(*s++) ++slen;
	s = append; while(*s++) ++alen;
	length = slen + alen + 1; // nul terminator
	if (!(buffer = malloc(sizeof(char) * length)))
		return 0;
	// Copy source
	s = source; d = buffer; while(*d++ = *s++) ; // empty body
	// rewind
	--d;
	// Copy append
	s = append;             while(*d++ = *s++) ; // empty body
	// terminating nul has already been copied

	return buffer;
}

static void run_procs () {
	schedule();
}

// Check if two strings match.
// Return codes:
//  0 - no match
//  1 - match
static int streq (char *a, char *b) {
	while(1) {
		if(*a == 0 && *b == 0) return 1;
		else if(*a != *b) return 0;
		++a; ++b;
	}
}

///
// Memcpy and memmove implementations
///
static void *sh_memcpy (void *source, void *dest, int length) {
	int   i;
	int  *is, *id;
	char *cs, *cd;

	i = 0;
	if((int)dest   % sizeof(int) == 0 &&
	   (int)source % sizeof(int) == 0 &&
	   length % sizeof(int) == 0) {
		is = source; id = dest;
		length = length / sizeof(int);
		while (i < length) { id[i] = is[i]; ++i; }
	} else {
		cs = source; cd = dest;
		while (i < length) { cd[i] = cs[i]; ++i; }
	}

	return dest;
}
void *sh_memmove (void *source, void *dest, int length) {
	int   i;
	int  *is, *id;
	char *cs, *cd;

	if ((int)dest < (int)source)
		return sh_memcpy(dest, source, length);

	i = length;
	if((int)dest   % sizeof(int) == 0 &&
	   (int)source % sizeof(int) == 0 &&
	   length % sizeof(int) == 0) {
		is = source; id = dest;
		length = length / sizeof(int);
		while (i > 0) { id[i - 1] = is[i - 1]; --i; }
	} else {
		cs = source; cd = dest;
		while (i > 0) { cd[i - 1] = cs[i - 1]; --i; }
	}

	return dest;
}

static void __memcpy_trailing_nul (char *dest, char *src, int sz) {
	while(sz-- > 0)
		*dest++ = *src++;
	*dest = 0;
}

///
// Environment functions
///
static char *env_find (char *name) {
	char *loc, *found, *namecheck;

	loc = environment;
	while(loc < env_max) {
		if(*loc == *name) {
			namecheck = name;
			while (*namecheck == *loc) { ++namecheck; ++loc; }
			if (*namecheck == 0 && *loc == '=') return loc + 1;
		}
		// find next item
		while (*loc++);
	}

	// not found
	return 0;
}

// TODO: this is incorrect
// Reproduction of error:
//   set foo 123
//   set bar foo
//   set foo 1
//   set
// ^ environment now corrupted
static void *env_set (char *name, char *value) {
	char *target;
	int   target_len, value_len;
	char *source, *dest, *new_max;

	value_len = strlen(value);

	if (!(target = env_find(name))) {
		target = env_max;
		// copy name over
		while(*name) *target++ = *name++;
		*target++ = '=';
		env_max = target + value_len + 1;
	} else {
		target_len = strlen(target);
		if (value_len < target_len) {
			// truncate: move data after value back
			source  = target + target_len + 1;
			dest    = target + value_len + 1;
			new_max = env_max - (target_len - value_len);
		} else {
			// expand: move data after value forward
			source  = target + value_len + 1;
			dest    = target + target_len + 1;
			new_max = env_max + (value_len - target_len);
		}
		sh_memmove(source, dest, env_max - dest);
	}

	// copy value over
	while(*value) *target++ = *value++;
	// copy nul
	*target++ = 0;
}

///
// Builtin commands interface
///

// Called by c4sh_builtins.c's constructor before main() runs.
// Sets up enough state to allow builtins to be added.
static int c4sh_builtin_initialized;
static int *c4sh_builtin_sort_temporary;

int c4sh_builtin_init () {
	int x;

	if (c4sh_builtin_initialized)
		return CR_OK;
	c4sh_builtin_initialized = 1;

	if (!(builtins = malloc((x = sizeof(int) * CMD__Sz * BUILTIN_LIMIT)))) {
		printf("c4sh builtin_init() failed to allocate %ld bytes\n", x);
		return CR_FAIL;
	} // else printf("c4sh builtin_init() allocated %ld bytes\n", x);
	if (!(c4sh_builtin_sort_temporary = malloc((x = sizeof(int) * CMD__Sz)))) {
		printf("c4sh: Failed to initialize builtin temporary\n");
		return CR_FAIL;
	}
	memset(builtins, 0, x);
	builtins_size = BUILTIN_LIMIT;
	builtins_used = 0;
	return CR_OK;
}

void c4sh_builtin_sort () {
	int a, b, bytes, maxA, maxB, *first, *second;
	char *s1, *s2;

	a = b = 0;
	first = builtins;
	maxA = builtins_used - 1;
	maxB = maxA - 1;
	bytes = sizeof(int) * CMD__Sz;

	while (a < maxA) {
		s1 = (char *)first[CMD_NAME];
		b = a;
		second = first + CMD__Sz;
		while (b < maxB) {
			if ((s2 = (char *)second[CMD_NAME])) {
				if (strcmp(s1, s2) > 0) {
					// swap to temporary
					memcpy(c4sh_builtin_sort_temporary, second, bytes);
					// copy from second to first
					memcpy(second, first, bytes);
					// copy from temporary to second
					memcpy(first, c4sh_builtin_sort_temporary, bytes);
				}
			}
			++b;
			second = second + CMD__Sz;
		}
		++a;
		first = first + CMD__Sz;
	}
}

int c4sh_builtin_register (char *name, char *short_desc, char *short_usage,
                           int *callback_usage, int *callback_run) {
	int id;
	int *builtin;
	if (builtins_used == builtins_size) {
		printf("c4sh: builtins table full, unable to add '%s'\n", name);
		return CR_FAIL;
	} // else printf("c4sh: adding builtin '%s'\n", name);
	id = builtins_used++;
	builtin = builtins + (CMD__Sz * id);
	builtin[CMD_NAME]        = (int) name;
	builtin[CMD_NAMELEN]     = strlen(name);
	builtin[CMD_DESC]        = (int) short_desc;
	builtin[CMD_USAGE_SHORT] = (int) short_usage;
	builtin[CMD_USAGE]       = (int) callback_usage;
	builtin[CMD_RUN]         = (int) callback_run;

	// TODO: under C4, this corrupts the builtins table. No issue under C4m.
	if (!(__c4_info() & C4I_C4))
		c4sh_builtin_sort();
	return CR_OK;
}

int *builtin_lookup (char *name) {
	int len, id, *builtin;

	id = 0;
	builtin = builtins;
	len = strlen(name);

	while(id < builtins_used) {
		if (len == builtin[CMD_NAMELEN] && !strcmp((char *)builtin[CMD_NAME], name))
			return builtin;
		++id;
		builtin = builtin + CMD__Sz;
	}

	return 0;
}

#define run(a,b) ((int (*)(int,char**))run)(a,b)
int builtin_run (int *builtin, int argc, char **argv) {
	int *run;
	run = (int *)builtin[CMD_RUN];
	return run(argc, argv);
}
#undef run

#define usage(a) ((void (*)(char *))usage)(a)
void builtin_usage (int *builtin, char *argv0) {
	int *usage;
	usage = (int *)builtin[CMD_USAGE];
	return usage(argv0);
}
#undef usage

///
// Shell functions
///
static int read_user_input () {
	// Copy previous user input
	//printf("Saving previous user input: %d '%s'\n", user_input_len, user_input);
	//memcpy(prev_input, user_input, BUFFER_SZ);
	sh_memcpy(user_input, prev_input, BUFFER_SZ);
	prev_input_len = user_input_len;
	//printf("Previous input: %d '%s'\n", prev_input_len, prev_input);

	memset(user_input, 0, BUFFER_SZ);
	user_input_len = read(STDIN, user_input, BUFFER_SZ);
	// printf("Read %ld bytes\n", user_input_len);
	if(user_input_len > 0)
		user_input[user_input_len - 1] = 0;

	// If was just the up arrow entered, recall prev_input
	if(*(user_input + 0) == 27 &&
	   *(user_input + 1) == 91 &&
	   *(user_input + 2) == 65) {
		//printf("Up arrow, restoring prev_input\n");
		//memcpy(user_input, prev_input, BUFFER_SZ);
		sh_memcpy(prev_input, user_input, BUFFER_SZ);
		user_input_len = prev_input_len;
		//printf("Prev input: %s\n", prev_input);
	} else {
		//printf("not an up arrow: %d %d %d\n", *(user_input + 0),
		//       *(user_input + 1), *(user_input + 2));
	}
	return user_input_len != 0;
}

enum { TAB = 9 };
static int  shell_is_space (char ch) {
	// printf("c4sh: is '%c' (%d) a space? %s\n", ch, ch, (ch == ' ' || ch == '\n') ? "yes" : "no");
	return ch == ' ' || ch == '\n' || ch == TAB;
}

static void shell_next () {
	// printf("shell_next: operating on %s\n", shell_text);
	while((shell_tk = shell_text) && *shell_tk) {
		++shell_text;
		// printf("c4sh: shell tk '%c' (%d)\n", *shell_tk, *shell_tk);
		if (*shell_tk == '#') { // Comment to end of line
			while (*shell_text && *shell_text != '\n') ++shell_text;
		} else if (*shell_tk == '&') {
			shell_tk_type = SHELL_BACKGROUND;
			return;
		// } else if (shell_is_space(*shell_tk) || *shell_tk == '\n') {
		} else if (*shell_tk == ' ' || *shell_tk == '\n' || *shell_tk == TAB) {
			// Do nothing
		} else if (*shell_tk == '"' || *shell_tk == '\'') { // Quote or double quote
			if (*shell_tk == '\'') shell_tk_type = SHELL_QUOTE;
			else if (*shell_tk == '"') shell_tk_type = SHELL_DBL_QUOTE;

			// Move to end of quote
			while (*shell_text && *shell_text != *shell_tk)
				++shell_text;
			if (*shell_text != *shell_tk) {
				printf("c4sh/next: expected '%c' closing quote but found '%c'\n",
				       *shell_tk, *shell_text);
			} else ++shell_text;
			// done
			return;
		} else {
			shell_tk_type = SHELL_TEXT;
			// Consider this a word, move to first space
			// while (*shell_text && !shell_is_space(*shell_text))
			while (*shell_text && !(*shell_text == ' ' || *shell_text == '\n'))
				++shell_text;
			// shell_tk has the token, shell_text is now at a space or end of line
			return;
		}
	}
	shell_tk = 0;
	shell_tk_type = SHELL_END;
}

static void foreground_process (int task_id) {
	int t0, t1, status;

	// Set the kernel focus task to the given one
	c4ke_set_focus(task_id);

	// If focus given back to ourselves, just return
	if (task_id == pid())
		return;

	if (!enable_ps_on_wait) {
		// TODO: c4ke doesn't restore await_pid status after a signal is sent.
		while (kern_task_running(task_id))
			await_pid(task_id); // blocking wait
		c4ke_set_focus(pid());
		return;
	}

	t0 = __time();
	while(kern_task_running(task_id)) {
		// printf("c4sh: Task still alive, scheduling...\n");
		schedule();
		// printf("c4sh: sleeping...\n");
		sleep(SLEEP_TIME);
		if (enable_ps_on_wait && kern_task_running(task_id)) {
			// Interval
			t1 = __time();
			if (t1 - t0 >= ps_interval) {
				//printf("c4sh: ps because t1(%d) - t0(%d) >= inter(%d)\n", t1, t0, ps_interval);
				ps();
				// debug_kernelstate();
				t0 = t1;
			}
		}
	}
	// Restore focus to us
	c4ke_set_focus(pid());
}

static void act_on_user_input() {
	int argc, task_id, words, i, sz;
	char *line;
	char *data, *data_start, **argv, *p;
	char *name;
	char *data_prev;
	int flags, y, nofree_name;
	int *b;

	line = user_input;
	flags = SHELLF_NONE;

	// First, count how many words we have
	argc = 0; shell_text = line; shell_next();
	while (shell_tk_type != SHELL_END) { shell_next(); ++argc; }
	shell_text = line; // reset
	// printf("c4sh: counted %d args\n", argc);

	if (argc == 0)
		return;

	if (!(argv = malloc(sizeof(char **) * argc))) {
		printf("c4sh: Failed to allocate argv pointer, aborting.\n");
		return;
	}
	// Allocate a buffer for data to copy argv values plus their trailing nul into
	if (!(data = data_start = malloc(SHELL_DATA_SIZE))) {
		printf("c4sh: Failed to allocate data pointer, aborting.\n");
		return;
	}
	memset(argv, 0, sizeof(char **) * argc);
	memset(data, 0, SHELL_DATA_SIZE);

	// Put pointer into argv, copy word and its trailing nul into data, advanced pointer
	i = 0; shell_next();
	while(shell_tk_type != SHELL_END) {
		if (shell_tk_type == SHELL_BACKGROUND) {
			flags = flags | SHELLF_BACKGROUND;
			// skip
			--argc;
		} else {
			data_prev = data;
			argv[i] = data;
			sz = shell_text - shell_tk;
			// printf("argv[%d] set to data (0x%lx), sz=%d\n", i, data, sz);
			if (shell_tk_type == SHELL_QUOTE) {
				// Copy between the quotes, no expansion
				// memcpy(data, shell_tk + 1, sz - 2);
				sz = sz - 2;
				__memcpy_trailing_nul(data, shell_tk + 1, sz);
			} else if (shell_tk_type == SHELL_DBL_QUOTE) {
				// Copy between the quotes, with expansion
				sz = sz - 2;
				// TODO: variable expansion
				__memcpy_trailing_nul(data, shell_tk + 1, sz);
			} else {
				// memcpy(data, shell_tk, sz);
				// y doesn't memcpy work?
				// TODO: variable expansion
				__memcpy_trailing_nul(data, shell_tk, sz);
				// printf("Copied from source: '%.*s'\n", sz, shell_tk);
			}
			//data = data + sz;
			//*data++ = 0; // nul
			// printf("Copied from source: '%s'\n", data_prev);
			data = data + sz + 1;
			++i;
		}
		shell_next();
	}

	// Ensure we actually have arguments, ie user hasn't simply entered '&'
	if (argc) {
		// Alias \q to exit
		// TODO: this should be moved elsewhere
		if (!strcmp("\\q", argv[0])) {
			argv[0] = "exit";
		}
		// Lookup builtins
		if ((b = builtin_lookup(argv[0]))) {
			builtin_run(b, argc, argv);
		} else {
			// Attempt to start a .c4r file
			// printf("c4sh: Arguments: %d\n", argc);
			// i = 0; while (i < argc) {
			//	printf("  argv[%d] = %s\n", i, argv[i]);
			//	++i;
			// }
			nofree_name = 0;
			if (!(name = strcpycat("task_loadc4r: ", argv[0]))) {
				printf("c4sh: failed to concatenate task name, using original\n");
				name = argv[0];
				nofree_name = 1;
			}
			// printf("c4sh: Starting '%s' with arguments...\n", name);
			task_id = kern_user_start_c4r(argc, argv, name, PRIV_USER);
			if (!task_id) {
				printf("c4sh: Failed to launch\n");
			} else {
				// printf("c4sh: Task %d started\n", task_id);
				last_pid = task_id;
				// ps();
				if (!(flags & SHELLF_BACKGROUND)) {
					// printf("c4sh: waiting for task to finish...\n");
					// printf("c4sh: task running %d? = %d\n", task_id, kern_task_running(task_id));
					foreground_process(task_id);
				} else {
					printf("c4sh: starting in background\n");
				}
			}
			if (!nofree_name) free(name);
		}
	}

	// We can free the argv and data now, as start_task_builtin copies what we pass in.
	free(argv);
	free(data_start);
}

void sig_start_success () {
	// ignore
}

void sig_start_failure () {
	// ignore
}

int main (int argc, char **argv) {
	// Print banner before initialization for slow computers
	version = "0.1";
	printf("\nC4SH - The C4 SHell v %s\nType help for command list\n", version);

	// Initialization
	currenttask_update_name("C4SH");
	if(!(user_input = malloc(BUFFER_SZ))) {
		printf("c4sh: Failed to allocate %ld bytes for user_input buffer\n", BUFFER_SZ);
		return 1;
	}
	if(!(prev_input = malloc(BUFFER_SZ))) {
		free(user_input);
		printf("c4sh: Failed to allocate %ld bytes for prev_input buffer\n", BUFFER_SZ);
		return 1;
	}
	if(!(environment = malloc(ENV_SZ))) {
		free(user_input);
		free(prev_input);
		printf("c4sh: Failed to allocate %ld bytes for environment buffer\n", ENV_SZ);
		return 2;
	}
	if (ps_init()) {
		free(user_input);
		free(prev_input);
		free(environment);
		printf("c4sh: Failed to initialize process interface\n");
		return -1;
	}

	memset(user_input, 0, BUFFER_SZ);
	memset(prev_input, 0, BUFFER_SZ);
	memset(environment, 0, ENV_SZ);

	signal(SIGUSR1, (int *)&sig_start_success);
	signal(SIGUSR2, (int *)&sig_start_failure);

	// Set defaults
	env_max = environment;
	//env_set("PATH", "c4-machine/bin:c4-machine");
	//env_set("PATH", "c4-machine/bin");
	env_set("PATH", "./");
	env_set("C4", "1");
	env_set("SHELL", "c4sh");
	enable_ps_on_wait = PS_ON_WAIT_DEFAULT;
	ps_interval = PS_INTERVAL_DEFAULT;
	shell_mode = SH_LOOP;
	user_prompt = "c4sh>\n"; // Newline required to flush buffer
	exit_code = 0;
	c4ke_set_focus(pid());

	// Shell loop
	while(shell_mode != SH_EXIT) {
		if(shell_mode == SH_LOOP) {
			// Prompt for more input
			printf(user_prompt);
			if(read_user_input()) {
				// Call schedule() first so that other task output displays
				schedule();
				act_on_user_input();
			} else {
				// CTRL+D or end of stream
				printf("c4sh: Lost input stream\n");
				shell_mode = SH_EXIT;
			}
		} else if(shell_mode == SH_PROC) {
			// Allow spawned processes to run
			run_procs();
		}
	}

	// Cleanup
	free(user_input);
	free(prev_input);
	free(environment);
	free(builtins);
	ps_uninit();
	printf("c4sh: exit code %d\n", exit_code);
	return exit_code;
}


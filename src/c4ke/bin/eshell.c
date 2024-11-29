//
// EShell - The emergency shell for C4KE.
//
// Provides a simple terminal interface for the C4 Kernel Experiment.
// Job control and supported commands are minimal.
//
// For a more useful shell, see c4sh.
//
// Compilation:
//  ./c4r u0.c eshell.c && mv a.c4r eshell.c4r
//  or let the Makefile do it for you as part of the run command:
//  ./make run
//
// Usage:
//  - Type "help" for a command listing.
//  - Any .c4r file can be run, and the .c4r extension does not need to be passed.
//  - You can start a task in the background using & like in bash:
//    top &
//    - kill not implemented
//    - you can foreground the most recently executed background task by using:
//      fg
//  - Any task running in the foreground causes eshell to wait, saving CPU cycles.
//  - Cursor keys do not work on the readline we have available. However, pressing
//    up and enter will run the last command again.
//  - #comments are ignored until end of line
//  - Double or single quotes can be used:
//    innerbench -Bn 10 -b "-b pi10"
//    - There is no text substitution in quotes, they merely group arguments together.
//    - Both quotes do the same thing, but you can "'nest quotes' for example".
//    - Quotes cannot be \"escaped\".

#include <c4.h>
#include <c4m.h>

#include <u0.h>
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
};

// Globals
static char *user_input, *user_prompt;
static int   user_input_len;
static char *prev_input;
static int   prev_input_len;
static int   shell_mode;
static char *version;
static char *environment, *env_max;
static int   last_pid;

static int   enable_ps_on_wait;
static int   ps_interval;

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

static char **c4_argv;
static int    c4_argc;
enum { SHELL_TEXT, SHELL_QUOTE, SHELL_DBL_QUOTE, SHELL_END, SHELL_BACKGROUND };
static char *shell_tk;
static int   shell_tk_type; // SHELL_*
static char *shell_text;
static int  shell_is_space (char ch) {
	// printf("eshell: is '%c' (%d) a space? %s\n", ch, ch, (ch == ' ' || ch == '\n') ? "yes" : "no");
	return ch == ' ' || ch == '\n';
}
static void shell_next () {
	// printf("shell_next: operating on %s\n", shell_text);
	while((shell_tk = shell_text) && *shell_tk) {
		++shell_text;
		// printf("eshell: shell tk '%c' (%d)\n", *shell_tk, *shell_tk);
		if (*shell_tk == '#') { // Comment to end of line
			while (*shell_text && *shell_text != '\n') ++shell_text;
		} else if (*shell_tk == '&') {
			shell_tk_type = SHELL_BACKGROUND;
			return;
		// } else if (shell_is_space(*shell_tk) || *shell_tk == '\n') {
		} else if (*shell_tk == ' ' || *shell_tk == '\n') {
			// Do nothing
		} else if (*shell_tk == '"' || *shell_tk == '\'') { // Quote or double quote
			if (*shell_tk == '\'') shell_tk_type = SHELL_QUOTE;
			else if (*shell_tk == '"') shell_tk_type = SHELL_DBL_QUOTE;

			// Move to end of quote
			while (*shell_text && *shell_text != *shell_tk)
				++shell_text;
			if (*shell_text != *shell_tk) {
				printf("eshell/next: expected '%c' closing quote but found '%c'\n",
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
enum { SHELL_DATA_SIZE = 4096 }; // Overkill?
enum { SHELLF_NONE, SHELLF_BACKGROUND = 1 };
static void __memcpy_trailing_nul (char *dest, char *src, int sz) {
	while(sz-- > 0)
		*dest++ = *src++;
	*dest = 0;
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
		// printf("eshell: Task still alive, scheduling...\n");
		schedule();
		// printf("eshell: sleeping...\n");
		sleep(SLEEP_TIME);
		if (enable_ps_on_wait && kern_task_running(task_id)) {
			// Interval
			t1 = __time();
			if (t1 - t0 >= ps_interval) {
				//printf("eshell: ps because t1(%d) - t0(%d) >= inter(%d)\n", t1, t0, ps_interval);
				ps();
				// debug_kernelstate();
				t0 = t1;
			}
		}
	}
	// Restore focus to us
	c4ke_set_focus(pid());
}
static void run_c4r (char *line) {
	int argc, task_id, words, i, sz;
	char *data, *data_start, **argv, *p;
	char *name;
	char *data_prev;
	int flags, y, nofree_name;

	flags = SHELLF_NONE;

	// First, count how many words we have
	argc = 0; shell_text = line; shell_next();
	while (shell_tk_type != SHELL_END) { shell_next(); ++argc; }
	shell_text = line; // reset
	// printf("eshell: counted %d args\n", argc);

	if (argc == 0)
		return;

	if (!(argv = malloc(sizeof(char **) * argc))) {
		printf("eshell: Failed to allocate argv pointer, aborting.\n");
		return;
	}
	// Allocate a buffer for data to copy argv values plus their trailing nul into
	if (!(data = data_start = malloc(SHELL_DATA_SIZE))) {
		printf("eshell: Failed to allocate data pointer, aborting.\n");
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
			if (shell_tk_type == SHELL_QUOTE || shell_tk_type == SHELL_DBL_QUOTE) {
				// Copy between the quotes
				// memcpy(data, shell_tk + 1, sz - 2);
				sz = sz - 2;
				__memcpy_trailing_nul(data, shell_tk + 1, sz);
			} else {
				// memcpy(data, shell_tk, sz);
				// y doesn't memcpy work?
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
		// printf("eshell: Arguments: %d\n", argc);
		// i = 0; while (i < argc) {
		//	printf("  argv[%d] = %s\n", i, argv[i]);
		//	++i;
		// }
		nofree_name = 0;
		if (!(name = strcpycat("task_loadc4r: ", argv[0]))) {
			printf("eshell: failed to concatenate task name, using original\n");
			name = argv[0];
			nofree_name = 1;
		}
		// printf("eshell: Starting '%s' with arguments...\n", name);
		task_id = kern_user_start_c4r(argc, argv, name, PRIV_USER);
		if (!task_id) {
			printf("eshell: Failed to launch\n");
		} else {
			// printf("eshell: Task %d started\n", task_id);
			last_pid = task_id;
			// ps();
			if (!(flags & SHELLF_BACKGROUND)) {
				// printf("eshell: waiting for task to finish...\n");
				// printf("eshell: task running %d? = %d\n", task_id, kern_task_running(task_id));
				foreground_process(task_id);
			} else {
				printf("eshell: starting in background\n");
			}
		}
		if (!nofree_name) free(name);
	}

	// We can free the argv and data now, as start_task_builtin copies what we pass in.
	free(argv);
	free(data_start);
}

static void dump_set () {
	char *e;
	int   len;
	e = environment;
	while(e < env_max) {
		if(*e) {
			len = strlen(e);
			printf(" %.*s\n", len, e);
			e = e + len + 1;
		} else ++e;
	}
}

static void do_set (char *expr) {
	if(strlen(expr) == 0) {
		dump_set();
	} else {
		printf("set: arguments not implemented: '%s' (%d)\n", expr, strlen(expr));
	}
}

static void act_on_user_input() {
	char *input;
	int  *task;
	int   i, max;
	int   pid, sig;

	max = 10;
    input = user_input;
	// Skip leading spaces
	while (*input == ' ') ++input;
	// Skip comments
	if (*input == '#') {
		while (*input && *input != '\n') ++input;
	}
	if (*input == 0) {
		// do nothing
		return;
	} else if(streq(input, "help")) {
		printf("Commands:\n"
		       "        help                Show this help\n"
		       "        exit (or \\q)        Exit the shell\n"
		       "        ps                  Show process listing\n"
		       "        fg [pid]            Bring a background process to foreground\n"
		       "        kill <pid> [signal] Send a signal to a process\n");
		printf("        toggle psf       Toggle ps during foreground process (default: %s)\n",
		       PS_ON_WAIT_DEFAULT ? "on" : "off");
		//printf("        enable psf       Enable ps during foreground process (default)\n");
		// printf("        test             Start a test program in the background\n");
		printf("        kdbg             Print kernel debug information\n");
		printf("        [cmd args]       Run given c4r file with provided arguments.\n");
		printf("        [cmd args] &     Run ... in the background\n");
	} else if(streq(input, "exit") || streq(input, "\\q")) {
		shell_mode = SH_EXIT;
	} else if(streq(input, "ps")) {
		ps();
	} else if(streq(input, "test")) {
		// printf("eshell: starting test task with %d tasks\n", max);
		// i = 0;
		// while(i++ < max && (task = start_task_builtin((int *)&task_printloop_2, 0, 0, "printloop2", PRIV_USER))) {
			// attach to task or something?
		// }
		// printf("eshell: %d tasks started\n", i - 1);
		printf("eshell: this feature has been removed\n");
	} else if(*input == 's' && *(input + 1) == 'e' && *(input + 2) == 't' &&
	          (*(input + 3) == 0 || *(input + 3) == ' ')) {
		do_set(input + 3);
	} else if(*(input + 0) == 't' && *(input + 1) == 'o' && *(input + 2) == 'g' &&
	          *(input + 3) == 'g' && *(input + 4) == 'l' && *(input + 5) == 'e' &&
			  *(input + 6) == ' ' &&
	          *(input + 7) == 'p' && *(input + 8) == 's' && *(input + 9) == 'f') {
		enable_ps_on_wait = !enable_ps_on_wait;
		printf("eshell: ps in foreground: %s\n", enable_ps_on_wait ? "on" : "off");
	} else if(*input == 'f' && *(input + 1) == 'g') {
		input = input + 2;
		// skip spaces
		while (*input == ' ') ++input;
		if (*input) {
			if (atoi_check(input, &i) != ATOI_OK) {
				printf("eshell: unable to understand '%s'\n", input);
			} else {
				foreground_process(i);
			}
		} else {
			if (last_pid == 0) {
				printf("eshell: no backgrounded task started yet, give a pid number\n");
			} else {
				foreground_process(last_pid);
			}
		}
	//} else if(!memcmp(input, "kdbg", 5)) {
	} else if(*input == 'k' && *(input + 1) == 'i' && *(input + 2) == 'l' && *(input + 3) == 'l') {
		// TODO: check for whitespace after 'kill'
		input = input + 4;
		while (*input == ' ') ++input;
		if (*input) {
			if (atoi_check(input, &pid) != ATOI_OK) {
				printf("eshell: unable to understand '%s'\n", input);
			} else {
				sig = SIGTERM; // default
				// find next space or end of line
				while (*input && *input != ' ') ++input;
				if (*input) {
					if (atoi_check(input, &sig) != ATOI_OK) {
						printf("eshell: unable to understand '%s' as a sig number (names not supported)\n",
						       input);
						sig = 0;
					}
				}
				if (sig) {
					kill(pid, sig);
					printf("eshell: sent signal %d to pid %d\n", sig, pid);
				}
			}
		}
	} else if(*(input + 0) == 'k' && *(input + 1) == 'd' && *(input + 2) == 'b' && *(input + 3) == 'g') {
		debug_kernelstate();
	} else {
		run_c4r(input);
	}
}

void sig_start_success () {
	// ignore
}

void sig_start_failure () {
	// ignore
}

int eshell_main (int argc, char **argv) {
	currenttask_update_name("eshell");

	// Banner
	version = "0.2";
	printf("\nc4ke emergency shell v %s\nType help for command list\n", version);

	// Initialization
	if(!(user_input = malloc(BUFFER_SZ))) {
		printf("Failed to allocate %ld bytes for user_input buffer\n", BUFFER_SZ);
		return 1;
	}
	if(!(prev_input = malloc(BUFFER_SZ))) {
		free(user_input);
		printf("Failed to allocate %ld bytes for prev_input buffer\n", BUFFER_SZ);
		return 1;
	}
	if(!(environment = malloc(ENV_SZ))) {
		free(user_input);
		free(prev_input);
		printf("Failed to allocate %ld bytes for environment buffer\n", ENV_SZ);
		return 2;
	}
	if (ps_init()) {
		free(user_input);
		free(prev_input);
		free(environment);
		printf("Failed to initialize process interface\n");
		return -1;
	}

	memset(user_input, 0, BUFFER_SZ);
	memset(prev_input, 0, BUFFER_SZ);
	memset(environment, 0, ENV_SZ);

	signal(SIGUSR1, (int *)&sig_start_success);
	signal(SIGUSR2, (int *)&sig_start_failure);

	// Set defaults
	// TODO: get rid of environments, feature never properly implemented.
	env_max = environment;
	//env_set("PATH", "c4-machine/bin:c4-machine");
	//env_set("PATH", "c4-machine/bin");
	env_set("PATH", "./");
	env_set("C4", "1");
	env_set("SHELL", "eshell");
	enable_ps_on_wait = PS_ON_WAIT_DEFAULT;
	ps_interval = PS_INTERVAL_DEFAULT;
	shell_mode = SH_LOOP;
	user_prompt = "c4ke eshell>\n"; // Newline required to flush buffer
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
				printf("Lost input stream\n");
				shell_mode = SH_EXIT;
			}
		}
	}

	// Cleanup
	free(user_input);
	free(prev_input);
	free(environment);
	ps_uninit();
	return 0;
}

#ifndef ESHELL_NOMAIN
int main (int argc, char **argv) { return eshell_main(argc, argv); }
#endif

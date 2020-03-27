// mesh - The C4 MachinE SHell
//
// Written for the c4 machine, designed to be compiled by cc.c
//
#include <stdio.h>
#include <stdlib.h>
#include <memory.h>

#ifdef __GNUC__
#include <unistd.h>
#else
#if _WIN64
#define __INTPTR_TYPE__ long long
#elif _WIN32
#define __INTPTR_TYPE__ int
#endif // if _WIN64
#endif // ifdef __GNUC__
#include <fcntl.h>
#define int __INTPTR_TYPE__

#include "machine.h" // for gcc/etc compatibility

// Streams
enum { STDIN, STDOUT, STDERR };

// Default values
enum { BUFFER_SZ = 1024, ENV_SZ = 31024 };

enum { // Shell modes
	SH_EXIT,
	SH_LOOP,
	SH_PROC
};

// Globals
char *user_input, *user_prompt;
int   user_input_len;
int   shell_mode;
char *version;
char *environment, *env_max;

int sh_strlen (char *s) { char *t; t = s; while(*t) ++t; return t - s; }

char *env_find (char *name) {
	char *loc, *found, *namecheck;

	loc = environment;
	while(loc < env_max) {
		if(*loc == *name) {
			namecheck = name;
			while(*namecheck++ == *loc++) ;
			if (*namecheck == *loc && *(loc + 1) == '=') return loc + 2;
		}
		// find next item
		while(loc < env_max && *loc) ++loc;
	}

	// not found
	return 0;
}

void *sh_memcpy (void *source, void *dest, int length) {
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
		while (i > 0) { cd[i - 1] = cs[i - 1]; ++i; }
	}

	return dest;
}

void *env_set (char *name, char *value) {
	char *target;
	int   target_len, value_len;
	char *source, *dest, *new_max;

	value_len = sh_strlen(value);

	if (!(target = env_find(name))) {
		target = env_max;
		// copy name over
		while(*name) *target++ = *name++;
		*target++ = '=';
	} else {
		target_len = sh_strlen(target);
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

int read_user_input () {
	user_input_len = read(STDIN, user_input, BUFFER_SZ);
	//printf("Read %ld bytes\n", user_input_len);
	if(user_input_len > 0)
		user_input[user_input_len - 1] = 0;
	return user_input_len != 0;
}

void run_procs () {
}

// Check if two strings match.
// Return codes:
//  0 - no match
//  1 - match
int streq (char *a, char *b) {
	while(1) {
		if(*a == 0 && *b == 0) return 1;
		else if(*a != *b) return 0;
		++a; ++b;
	}
}

// For direct compilation
#define main c4_main
#include "c4.c"
#undef main
#undef c4_main

char **c4_argv;
int    c4_argc;
void open_c4 (char *name) {
	int args;
	char *c, *start, **tmp;

	// first, count args
	args = 128;

	//printf("open_c4('%s')\n", name);

	c4_argv = malloc(sizeof(char*) * args);
	c4_argv[0] = "c4";
	c4_argv[1] = name;
	args = 2;
	c = name; while(*c && *c != ' ') ++c;
	if(*c == ' ') {
		*c = 0; ++c;
		while(*c == ' ') ++c;
		start = c;
		while(*c) {
			if(*c == ' ') {
				*c++ = 0; ++c;
				while(*c == ' ') ++c; --c;
				c4_argv[args++] = start;
				start = c;
			} else ++c;
		}
		if(start != c)
			c4_argv[args++] = start;
	}
	c4_argc = args;

	// Debug
	if(0) {
		printf("Arg count: %ld\n", c4_argc);
		args = 0;
		while(args < c4_argc) {
			printf("arg[%ld] = '%s'\n", args, c4_argv[args]);
			++args;
		}
	}

	c4_main(c4_argc, c4_argv);
	free(c4_argv);
}

void open_script (char *name) {
	int fd, bufsize, bytes;
	char *buf;

	bufsize = 256 * 1024;
	if(!(buf = malloc(bufsize))) {
		printf("Failed to allocate %ld bytes for file buffer\n", bufsize);
		return;
	}

	if((fd = open(name, 0)) < 0) {
		printf("Could not open: %s\n", name);
		return;
	}
	if((bytes = read(fd, buf, bufsize)) <= 0) {
		printf("read() returned %ld\n", bytes);
		free(buf);
		close(fd);
		return;
	}

	free(buf);
	close(fd);
}

void bluepill () {
	c4_argc = 3;
	c4_argv = malloc(sizeof(char*) * c4_argc);
	c4_argv[0] = "c4";
	c4_argv[1] = "c4.c";
	c4_argv[2] = "sh.c";
	printf("Diving deeper into the simulation...\n");
	c4_main(c4_argc, c4_argv);
	free(c4_argv);
}

void dump_set () {
	char *e;
	int   len;
	e = environment;
	while(e < env_max) {
		if(*e) {
			len = sh_strlen(e);
			printf(" %.*s\n", len, e);
			e = e + len;
		} else ++e;
	}
}

void do_set (char *expr) {
	if(sh_strlen(expr) == 0) {
		dump_set();
	} else {
		printf("set: arguments not implemented: '%s' (%d)\n", expr, sh_strlen(expr));
	}
}

void act_on_user_input() {
    char *input;

    input = user_input;
	// Skip leading spaces
	while (*input == ' ') input++;
	// Skip comments
	if (*input == '#') {
		while (*input && *input != '\n') ++input;
	}
	if (*input == 0) {
		// do nothing
		return;
	} else if(streq(input, "help")) {
		printf("Commands:\n");
		printf("        help             Show this help\n");
		printf("        exit             Exit the shell\n");
		printf("        blue             Take the blue pill and dive deeper\n");
		printf("        [anything else]  Run given file through c4\n");
	} else if(streq(input, "blue")) {
		bluepill();
	} else if(streq(input, "exit")) {
		shell_mode = SH_EXIT;
	} else if(*input == 's' && *(input + 1) == 'e' && *(input + 2) == 't' &&
	          (*(input + 3) == 0 || *(input + 3) == ' ')) {
		do_set(input + 3);
	} else {
		open_c4(input);
	}
}

int main (int argc, char **argv) {
	if(!(user_input = malloc(BUFFER_SZ))) {
		printf("Failed to allocate %ld bytes for user_input buffer\n", BUFFER_SZ);
		return 1;
	}
	if(!(environment = malloc(ENV_SZ))) {
		printf("Failed to allocate %ld bytes for environment buffer\n", ENV_SZ);
		return 2;
	}

	memset(user_input, 0, BUFFER_SZ);
	memset(environment, 0, ENV_SZ);
	env_max = environment;
	env_set("PATH", "c4-machine/bin:c4-machine");

	version = "0.2";
	shell_mode = SH_LOOP;
	user_prompt = "c4sh>\n"; // Newline required to flush buffer
	printf("C4SH v %s\nType help for command list\n", version);
	while(shell_mode != SH_EXIT) {
		if(shell_mode == SH_LOOP) {
			printf(user_prompt);
			if(read_user_input()) {
				act_on_user_input();
			} else {
				printf("Lost input stream\n");
				shell_mode = SH_EXIT;
			}
		} else if(shell_mode == SH_PROC) {
			run_procs();
		}
	}

	free(user_input);
	free(environment);
	return 0;
}


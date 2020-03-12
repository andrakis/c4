// C4SH - The C4 SHell
//
// Written in Strict-C4 compliance.
//
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <memory.h>

// Streams
enum { STDIN, STDOUT, STDERR };

enum { BUFFER_SZ = 1024 };

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
	if(c4_depth() == 0) {
		c4_argc = 2;
		c4_argv[1] = "sh.c";
	} else {
		c4_argv[1] = "c4.c";
		c4_argv[2] = "sh.c";
	}
	printf("Diving deeper into the simulation...\n");
	c4_main(c4_argc, c4_argv);
	free(c4_argv);
}

void act_on_user_input() {
    char *input;

    input = user_input;
	// Skip leading spaces
	while(*input == ' ') input++;
	if(*input == 0) {
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
	} else {
		open_c4(input);
	}
}

int main (int argc, char **argv) {
	if(!(user_input = malloc(sizeof(char) * BUFFER_SZ))) {
		printf("Failed to allocate %ld bytes for user_input buffer\n", sizeof(char) * BUFFER_SZ);
		return 1;
	}

	version = "0.2";
	shell_mode = SH_LOOP;
	user_prompt = "c4sh>\n"; // Newline required to flush buffer
	printf("C4SH v %s depth %ld\nType help for command list\n", version, c4_depth());
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
	return 0;
}

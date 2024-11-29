///
/// C4LE - The C4 Line Editor
///

// HOWTO:
//  c4le filename
//
//  Prompts:
//   c4le nnn>    Command mode at line nnn
//   a nnn>       Append mode at line nnn
//   e nnn>       Edit mode at line nnn
//   i nnn>       Insert mode at line nnn
// In append or edit mode, typing \ by itself returns to command mode.
// In command mode the following commands are accepted:
//
// a          Append a new line to the end of the file and edit it
// e          Edit the current line
// e nnn      Edit line nnn
// i nnn      Insert a new line at line nnn
// s n1 n2    Swap line n1 and n2 with each other
// l          List the file
// l 10-      List the file from line 10 onwards
// l -10      List the last 10 lines of the file
// l 10-20    List lines 10 to 20, inclusive
// w          Write to the file specified in current filename
// w file     Write to given filename
// q          Quit
// wq         Write and quit
// o file     Open given file and update current filename

#include "c4.h"
#include "c4m.h"
#include "u0.c"

// Streams
enum { STDIN, STDOUT, STDERR }; // TODO: move to u0.c

// Command structure
enum {
	CMD_KEYWORD,      // char *, the keyword, eg "a", "e", "l", ...
	CMD_HANDLER,      // int (*)(...), the function pointer to the command handler
	CMD__Sz
};

int *commands, commands_count;
enum { COMMANDS_MAX = 16 };
int add_command (char *keyword, int *handler) {
	int *cmd;

	cmd = commands + (CMD__Sz * commands_count++);
	cmd[CMD_KEYWORD] = (int)keyword;
	cmd[CMD_HANDLER] = (int)handler;

	return 0;
}

//
// Editor state
//
char *file_contents;  // Entire file
char *file_before;    // Before current line
char *file_current;   // Current line being edited
char *file_after;     // After current line

//
// Editor commmands
//
int cmd_append () { }
int cmd_edit () { }
int cmd_insert () { }
int cmd_swap () { }
int cmd_list () { }
int cmd_open () { }
int cmd_write () { }
int cmd_quit () { }
int cmd_writequit () { }

int main (int argc, char **argv) {
	if (!(commands = malloc(sizeof(int) * CMD__Sz * COMMANDS_MAX))) {
		printf("c4le: memory allocation failure\n");
		return -1;
	}

	add_command("a", (int *)&cmd_append);
	add_command("e", (int *)&cmd_edit);
	add_command("i", (int *)&cmd_insert);
	add_command("s", (int *)&cmd_swap);
	add_command("l", (int *)&cmd_list);
	add_command("o", (int *)&cmd_open);
	add_command("w", (int *)&cmd_write);
	add_command("q", (int *)&cmd_quit);
	add_command("wq", (int *)&cmd_writequit);

	free(commands);
	return 0;
}

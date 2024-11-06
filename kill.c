// EShell Command: kill
//
// Send a signal to a job.
// This command is compiled after eshell.c, and adds its command via a
// constructor.
//
// See usage() below.

#include "eshell.c"

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
//"       -s sig    SIG is a signal name\n"
"       -n sig    SIG is a signal number\n"
"       -l        list the signal names; if arguments follow `-l' they are\n"
"                 assumed to be signal numbers for which names should be listed\n"
"       -L        synonym for -l\n"
"\n"
"     Kill is a shell builtin for two reasons: it allows job IDs to be used\n"
"     instead of process IDs, and allows processes to be killed if the limit\n"
"     on processes that you can create is reached.\n"
"\n"
"     Exit Status:\n"
"     Returns success unless an invalid option is given or an error occurs.\n");
}

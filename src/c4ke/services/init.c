// C4KE process: init
//
// Handles starting various services and the shell.
// Builds with eshell to provide an emergency shell if the init process dies.
//

#include <u0.h>

#define ESHELL_NOMAIN 1
#include "eshell.c"

enum { SERVICE_START_WAIT_TIME = 5000,
       SERVICE_START_SLEEP_TIME = 100 };

int pid_vfs;

int shell_argc;
char **shell_argv;

int current_service_pid, current_service_signal;

static void sig_usr_handler (int sig) {
	// printf("init: sig_usr_handler(%d / 0x%x)\n", sig, sig);
	current_service_signal = sig;
}

static void sig_usr1_handler () { sig_usr_handler(SIGUSR1); }
static void sig_usr2_handler () { sig_usr_handler(SIGUSR2); }

int start_service (int argc, char **argv, char *name) {
	int pid;
	int start_time;
	int alive;
	int result;

	result = 0;
	if (!(pid = kern_user_start_c4r(shell_argc, shell_argv, name, PRIV_KERNEL))) {
		printf("init: ...failed\n");
	} else {
		// printf("init: waiting for success or failure...\n");
		current_service_pid = pid;
		current_service_signal = 0;
		alive = 1;
		start_time = __time();
		while(alive && current_service_signal == 0 && (__time() - start_time) < SERVICE_START_WAIT_TIME) {
			alive = kern_task_running(pid);
			sleep(SERVICE_START_SLEEP_TIME);
		}
		if ((__time() - start_time) >= SERVICE_START_WAIT_TIME) {
			printf("init: ... failure, timed out waiting on start after %dms\n", SERVICE_START_WAIT_TIME);
		} else if (!alive) {
			printf("init: ... failure, process died\n");
		} else if (current_service_signal == SIGUSR1) {
			printf("init: ... success\n");
			result = 1;
		} else {
			printf("init: ... failure\n");
		}
	}

	return result;
}

int start_services () {
	int failures;

	// install service status handlers
	signal(SIGUSR1, (int *)&sig_usr1_handler);
	signal(SIGUSR2, (int *)&sig_usr2_handler);

	failures = 0;

	// TODO: testing more services
	if (0) {
		// Start top
		shell_argc = 2;
		shell_argv[0] = "top";
		shell_argv[1] = "-b";
		failures = failures + (start_service(shell_argc, shell_argv, "service: top") ? 0 : 1);
	}

	// Start VFS
	shell_argc = 1;
	shell_argv[0] = "c4ke.vfs";
	printf("init: starting VFS...\n");
	// Add to failure count if unable to start successfully
	failures = failures + (start_service(shell_argc, shell_argv, "service: vfs") ? 0 : 1);

	// remove service status handlers
	signal(SIGUSR1, 0);
	signal(SIGUSR2, 0);

	return failures;
}

int main (int argc, char **argv) {
	int i, pid;
	int start_eshell;

	currenttask_update_name("init");

	start_eshell = 0;

	shell_argc = 1;
	if (!(shell_argv = malloc(128))) { // TODO: better
		printf("init: failed to allocate shell argv, dropping back to emergency shell\n");
		start_eshell = 1;
	}
	if (shell_argv) memset(shell_argv, 0, 128);
	if ((i = start_services())) {
		printf("init: %d services failed to start\n", i);
		// TODO: do we need to start_eshell = 1; here?
	}

	// Arguments on the command line? Attempt to start that
	if (argc > 1) {
		if (!(pid = kern_user_start_c4r(argc - 1, argv + 1, *(argv + 1), PRIV_KERNEL))) {
			printf("init: failed to spawn given init process: '%s'\n", *(argv + 1));
			start_eshell = 1;
		} else {
			printf("init: custom init '%s' started as pid %d\n", *(argv + 1), pid);
		}
	} else {
		// Start the regular shell
		shell_argv[0] = "c4sh";
		if (!(pid = kern_user_start_c4r(shell_argc, shell_argv, "c4sh", PRIV_USER))) {
			printf("init: failed to spawn shell\n");
			start_eshell = 1;
		} else {
			printf("init: shell '%s' started as pid %d\n", *shell_argv, pid);
		}
	}

	if (!pid || (i = await_pid(pid))) {
		start_eshell = 1;
	}

	if (start_eshell) {
		// Start the emergency shell
		printf("init: failed, starting builtin emergency shell\n");
		i = eshell_main(shell_argc, shell_argv);
	}

	printf("init: task completed with code %d\n", i);
	return i;
}

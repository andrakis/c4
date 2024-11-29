//
// VFS Service for C4KE
//
//

#include "c4.h"
#include "service.h"
#include "u0.c"

enum { MSG_TIMEOUT = 5000 };
int vfs_pid, instance;
int run;

int message_handler (int *sender, int size, char *message) {
	c4ke_message_dump(sender, size, message);
	return c4ke_message_handled(sender);
}

// TODO: this signal handler not getting called
void sighandler_term (int sig) {
    printf("vfs%d: shutdown request received\n", instance);
    // run = 1; // TEST: don't comply
    run = 0;
}

int main (int argc, char **argv) {
	int tasks, *t, i;
	char *name, *prefix, *s1, *s2;
	int *msg;

	// instance = vfs_instances++;
	vfs_pid = pid();
	instance = 0;
	printf("vfs%d: process %d starting...\n", instance, vfs_pid);
	// Create new name with instance id attached
	prefix = "kernel/vfs";
	if (!(name = malloc((i = strlen(prefix) + itoa_len(instance) + 1)))) {
		printf("vfs%d: memory allocation failure of %d bytes\n", instance, i);
		return 1;
	}
	// Copy the prefix
	s1 = prefix; s2 = name; while(*s2++ = *s1++) ; --s2;
	// Copy the number - only supporting one for now
	*s2++ = '0' + instance;
	*s2++ = 0;
	currenttask_update_name(name);
	// We can now free the name
	free(name);

    // setup SIGTERM handler
    signal(SIGTERM, (int *)&sighandler_term);
	
	// TODO: load c4ke.vfs.txt

	// send a signal to the parent we're ready
	kill(parent(), SIGUSR1);
	//kill(parent(), SIGUSR2); // report failure
	//exit(1);                 // fake a crash

	printf("vfs%d: entering message loop...\n", instance);
	run = 1;
	while(run) {
		// wait for a message...
		if ((msg = await_message(MSG_TIMEOUT))) {
			printf("vfs%d: got message addr 0x%x\n", instance, msg);
		} else {
			// printf("vfs%d: message timeout\n", instance);
		}
	}

	printf("vfs%d: syncing filesystems...\n", instance);
	//__c4_opcode(500, OP_USER_SLEEP);
	printf("vfs%d: sync complete.\n", instance);
	return 0;
}

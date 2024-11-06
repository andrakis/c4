//
// Header file for services
//
//

#include <stdio.h>

enum {
	MSG_HANDLED,
	MSG_REJECTED,
	MSG_REQUEUE
};

static int c4ke_message_handled (int *sender) { return MSG_HANDLED; }
static int c4ke_message_rejected (int *sender) { return MSG_REJECTED; }
static int c4ke_message_requeue (int *sender) { return MSG_REQUEUE; }

static void c4ke_message_dump_sender (int *sender) {
	// for now it's just a PID
	printf("<.%ld>", (int)sender);
}
static void c4ke_message_dump (int *sender, int size, char *message) {
	printf("c4ke: message dump: from sender ");
	c4ke_message_dump_sender(sender);
	printf("\n    size: %ld\n", size);
	printf("\n    message: ");
	if (size <= 64) printf("%s\n", message);
	else printf("%.64s...\n", message);
}

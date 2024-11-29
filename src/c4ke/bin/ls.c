// C4 Ls
//
// For now, just prints what I want it to

#include <stdio.h>
#include "c4.h"

// These enums must match c4ke's
enum {
	MSGS_REJECT  = 0x00, // Message rejected, optionally notifying. Message will be dropped.
	MSGS_REQUEUE = 0x01, // Reject and requeue. Message goes to back of line
	MSGS_REQUEUE_FRONT = 0x02, // Requeue to front
	MSGS_REQUEUE_ELSEWHERE = 0x04, // Requeue but don't send to this process again.
	MSGS_ACCEPT  = 0x10, // Message accepted, optionally notifying. Message will be dropped.
	MSGS_HOLD    = 0x20  // Message will be held for now, optionally notifying.
};

enum {
	MSG_SENDER,
	MSG_TYPE,
	MSG_ID,
	MSG_LENGTH,
	MSG_DATA
};

// Dummy out for now. TODO: move to u0 and call kernel function
static int msg_select (int **msg, int *tv) { return 0; }
static int *timeval (int ms) { return 0; }
static void tv_free (int *tv) { }
// End dummy


static int done;
static int on_message (int sender, int type, int id, int length, char *message) {
	printf("message(%ld).%ld from %ld, %ld in length\n", type, sender, length);
	// TODO: this goes nowhere
	return MSGS_ACCEPT;
}

static void message_loop () {
	int *tv, *msg, r;

	tv = timeval(5000);
	while(!done) {
		if ((r = msg_select(&msg, tv))) {
			on_message(msg[MSG_SENDER], msg[MSG_TYPE], msg[MSG_ID], msg[MSG_LENGTH], (char *)msg[MSG_DATA]);
		}
	}
	tv_free(tv);
}

int main (int argc, char **argv) {
	printf(
		"bench     c4rdump        hello       ps          test_customop      test_signal\n"
		"benchtop  c4rlink        init        spin        test_exit          test_static\n"
		"c4cc      c4sh           innerbench  test-order  test_fread         tests\n"
		"c4ke      cat            kill        test-ptrs   test_infiniteloop  top\n"
		"c4ke.vfs  echo           ls          test_args   test_malloc        type\n"
		"c4le      factorial      mandel      test_basic  test_printf        xxd\n"
		"c4m       fun_with_ptrs  multifun    test_crash  test_printloop\n"
	);

	return 0;
}

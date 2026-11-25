// mbpair.c - two C4KE tasks talking through their inboxes. The parent starts
// a child (this same program with an argument), waits for a message, and
// prints what it got and from whom; the child sends three words to its parent.

int main (int argc, char **argv) {
	int buf[16], payload[3], n, pid;
	char *child_argv[2];
	if (argc > 1) {                                  // the child
		payload[0] = 10; payload[1] = 20; payload[2] = 30;
		if (!msg_send(parent(), 7, payload, 3)) { printf("mbpair-child: send failed\n"); return 1; }
		return 0;
	}
	child_argv[0] = "mbpair.c4r"; child_argv[1] = "child";
	if (!(pid = kern_user_start_c4r(2, child_argv, "mbpair-child", PRIV_USER))) { printf("mbpair: cannot start the child\n"); return 1; }
	if (!mbox_await(5000)) { printf("mbpair: timed out\n"); return 1; }
	n = msg_recv(buf, 16);
	printf("mbpair: got type %d from pid %d (%d words): %d %d %d\n", buf[1], buf[2], n - 3, buf[3], buf[4], buf[5]);
	await_pid(pid);
	return 0;
}

// c4ke_mbox.h -- the mailbox from a C4KE task (extensions/c4ke_mbox.c).
// Pass as a source after u0.h (c4cc has no #include). Opcodes are resolved
// by name at start, u0's way, so their numbers are the kernel's to choose.

static int OP_MBOX_INFO, OP_MBOX_SEND, OP_MBOX_RECV, OP_MBOX_AWAIT, OP_MSG_SEND, OP_MSG_RECV;

static int __attribute__((constructor)) __mbox_ops_init () {
	OP_MBOX_INFO  = __c4_opcode("OP_MBOX_INFO",  OP_REQUEST_SYMBOL);
	OP_MBOX_SEND  = __c4_opcode("OP_MBOX_SEND",  OP_REQUEST_SYMBOL);
	OP_MBOX_RECV  = __c4_opcode("OP_MBOX_RECV",  OP_REQUEST_SYMBOL);
	OP_MBOX_AWAIT = __c4_opcode("OP_MBOX_AWAIT", OP_REQUEST_SYMBOL);
	OP_MSG_SEND   = __c4_opcode("OP_MSG_SEND",   OP_REQUEST_SYMBOL);
	OP_MSG_RECV   = __c4_opcode("OP_MSG_RECV",   OP_REQUEST_SYMBOL);
	return 0;
}

// arguments are listed in reverse: __c4_opcode pushes left to right and the
// kernel reads sp[1] as the first argument (u0.h)
int mbox_info ()                              { return __c4_opcode(OP_MBOX_INFO); }
int mbox_send (int type, int *payload, int n) { return __c4_opcode(n, payload, type, OP_MBOX_SEND); }
int mbox_recv (int *buf, int max)             { return __c4_opcode(max, buf, OP_MBOX_RECV); }
int mbox_await (int timeout_ms)               { return __c4_opcode(timeout_ms, OP_MBOX_AWAIT); }
int msg_send (int pid, int type, int *payload, int n) { return __c4_opcode(n, payload, type, pid, OP_MSG_SEND); }
int msg_recv (int *buf, int max)              { return __c4_opcode(max, buf, OP_MSG_RECV); }

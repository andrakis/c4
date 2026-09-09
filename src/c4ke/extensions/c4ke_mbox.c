// C4KE extension: the mailbox -- frames between the host and tasks, and
// between tasks.
//
// Host side (c4bb's MBOX_* device, docs/c4bb-design.md "The mailbox"): two
// rings in RAM. The first task to receive or await host frames becomes their
// owner; frames stay in the host ring until that task takes them, so the
// scheduler can see them waiting (kernel_task_find_real, WSTATE_MESSAGE).
//
// Task side: every task may have an inbox, a ring of [len][type][sender]
// [payload...] frames allocated on first delivery and freed with the task
// (kernel_clean_task). A send wakes a receiver blocked in OP_MBOX_AWAIT.
//
// Opcodes, resolved by name through OP_REQUEST_SYMBOL (include/c4ke_mbox.h):
//   OP_MBOX_INFO ()                         1 when the host mailbox is fitted
//   OP_MBOX_SEND (type, payload, n)         a frame to the host; rings REPLY
//   OP_MBOX_RECV (buf, max)                 the next host frame into buf -> its length in words, or 0
//   OP_MBOX_AWAIT (timeout_ms, 0 = never)   block until a host frame (owner) or a task message -> 1, timeout -> 0
//   OP_MSG_SEND (pid, type, payload, n)     a frame into pid's inbox -> 1, or 0 (no such pid / full)
//   OP_MSG_RECV (buf, max)                  the next inbox frame -> its length, or 0; buf[2] is the sender pid
//
// Inert on a host without C4I_MBOX (native c4m): OP_MBOX_INFO says 0 and the
// task-to-task opcodes still work.

enum { MBX_INBOX_WORDS = 1024, MBX_NEVER = 2147483647 };
enum { MBX_OP_BASE = 248 };   // CO_BASE + CO_MAX - 8: the top of the custom range, clear of the kernel's own

static int OP_MBOX_INFO_, OP_MBOX_SEND_, OP_MBOX_RECV_, OP_MBOX_AWAIT_, OP_MSG_SEND_, OP_MSG_RECV_;

// ---- a ring of frames, the same shape as the host's ([cap][head][tail][data]) ----
static int *mbx_ring_peek (int *r) {
	int cap, head, tail, at;
	cap = r[0]; head = r[1]; tail = r[2];
	if (head == tail) return 0;
	at = tail % cap;
	if (r[3 + at] == -1) { tail = tail + (cap - at); r[2] = tail; if (head == tail) return 0; at = 0; }
	return r + 3 + at;
}
static void mbx_ring_take (int *r, int *f) { r[2] = r[2] + f[0]; }
static int mbx_ring_put (int *r, int type, int sender, int *payload, int n) {
	int cap, head, tail, at, len, free, i, *f;
	cap = r[0]; head = r[1]; tail = r[2];
	len = 3 + n; free = cap - (head - tail); at = head % cap;
	if (at + len > cap) {
		if (free < (cap - at) + len) return 0;
		r[3 + at] = -1; head = head + (cap - at); at = 0; free = cap - (head - tail);
	}
	if (free < len) return 0;
	f = r + 3 + at;
	f[1] = type; f[2] = sender;
	i = 0; while (i < n) { f[3 + i] = payload[i]; i = i + 1; }
	f[0] = len; r[1] = head + len;
	return 1;
}
static int *mbx_inbox (int *t) {
	int *r;
	if (!t[TASK_MBOX]) {
		if (!(r = (int *)malloc(sizeof(int) * (3 + MBX_INBOX_WORDS)))) return 0;
		r[0] = MBX_INBOX_WORDS; r[1] = 0; r[2] = 0;
		t[TASK_MBOX] = (int)r; t[TASK_MBOX_SZ] = MBX_INBOX_WORDS; t[TASK_MBOX_COUNT] = 0;
	}
	return (int *)t[TASK_MBOX];
}

// ---- the opcodes ------------------------------------------------------------
static void op_mbox_info (int trap, int ins, int mode, int a, int *bp, int *sp, int *returnpc) {
	a = mbx_fitted;
	trap_exit();
}
static void op_mbox_send (int trap, int ins, int mode, int a, int *bp, int *sp, int *returnpc) {
	if (!mbx_fitted) a = 0;
	else {
		a = mb_send(sp[1], (int *)sp[2], sp[3]);
		mb_bell(MB_BELL_REPLY);
	}
	trap_exit();
}
static void op_mbox_recv (int trap, int ins, int mode, int a, int *bp, int *sp, int *returnpc) {
	int *buf, max, *f, n, i;
	buf = (int *)sp[1]; max = sp[2];
	a = 0;
	if (mbx_fitted) {
		if (!mbx_host_owner) mbx_host_owner = kernel_task_current;
		if (mbx_host_owner == kernel_task_current && (f = mb_poll())) {
			n = f[0]; if (n > max) n = max;
			i = 0; while (i < n) { buf[i] = f[i]; i = i + 1; }
			mb_done(f);
			a = f[0];
		}
	}
	trap_exit();
}
static void op_mbox_await (int trap, int ins, int mode, int a, int *bp, int *sp, int *returnpc) {
	int timeout;
	timeout = sp[1];
	if (mbx_fitted && !mbx_host_owner) mbx_host_owner = kernel_task_current;
	if (kernel_task_current[TASK_MBOX_COUNT] > 0 ||
	    (mbx_host_owner == kernel_task_current && mbx_host_pending())) {
		a = 1;                          // something is already there
		trap_exit();
		return;
	}
	*kernel_task_current = *kernel_task_current | STATE_WAITING;
	kernel_task_current[TASK_WAITSTATE] = WSTATE_MESSAGE;
	kernel_task_current[TASK_WAITARG] = timeout > 0 ? kernel_last_time + timeout : 0;   // 0 = never
	kernel_task_current[TASK_REG_A] = 0;
	++kernel_tasks_waiting;
	// the scheduler puts 1 (a message) or 0 (timed out) into TASK_REG_A on wake
	__c4_jmp((int *)&trap_schedule_in_trap + 2);
}
static void op_msg_send (int trap, int ins, int mode, int a, int *bp, int *sp, int *returnpc) {
	int *t, *r;
	a = 0;
	if ((t = kernel_task_find_pid(sp[1])) && (r = mbx_inbox(t))) {
		if (mbx_ring_put(r, sp[2], kernel_task_current[TASK_ID], (int *)sp[3], sp[4])) {
			t[TASK_MBOX_COUNT] = t[TASK_MBOX_COUNT] + 1;
			if ((t[TASK_STATE] & STATE_WAITING) && t[TASK_WAITSTATE] == WSTATE_MESSAGE) {
				t[TASK_REG_A] = 1;
				kernel_task_wake(t);
			}
			a = 1;
		}
	}
	trap_exit();
}
static void op_msg_recv (int trap, int ins, int mode, int a, int *bp, int *sp, int *returnpc) {
	int *buf, max, *r, *f, n, i;
	buf = (int *)sp[1]; max = sp[2];
	a = 0;
	if ((r = (int *)kernel_task_current[TASK_MBOX]) && (f = mbx_ring_peek(r))) {
		n = f[0]; if (n > max) n = max;
		i = 0; while (i < n) { buf[i] = f[i]; i = i + 1; }
		mbx_ring_take(r, f);
		kernel_task_current[TASK_MBOX_COUNT] = kernel_task_current[TASK_MBOX_COUNT] - 1;
		a = f[0];
	}
	trap_exit();
}

// init runs BEFORE the kernel allocates custom_opcodes (kernel main), so the
// names are registered here and the handlers installed from start, which runs
// after the kernel's own opcodes are in.
static int mbox_init () {
	mbx_fitted = mb_init();           // announced, never probed: mb_init asks INFO first
	mbx_host_owner = 0;
	OP_MBOX_INFO_  = MBX_OP_BASE;
	OP_MBOX_SEND_  = MBX_OP_BASE + 1;
	OP_MBOX_RECV_  = MBX_OP_BASE + 2;
	OP_MBOX_AWAIT_ = MBX_OP_BASE + 3;
	OP_MSG_SEND_   = MBX_OP_BASE + 4;
	OP_MSG_RECV_   = MBX_OP_BASE + 5;
	kext_register_symbol("OP_MBOX_INFO",  OP_MBOX_INFO_);
	kext_register_symbol("OP_MBOX_SEND",  OP_MBOX_SEND_);
	kext_register_symbol("OP_MBOX_RECV",  OP_MBOX_RECV_);
	kext_register_symbol("OP_MBOX_AWAIT", OP_MBOX_AWAIT_);
	kext_register_symbol("OP_MSG_SEND",   OP_MSG_SEND_);
	kext_register_symbol("OP_MSG_RECV",   OP_MSG_RECV_);
	if (kernel_verbosity >= VERB_MED)
		printf("c4ke: mbox module loaded, host mailbox %s\n", mbx_fitted ? "fitted" : "absent");
	return KXERR_NONE;
}
static int mbox_start () {
	install_custom_opcode(OP_MBOX_INFO_,  (int *)&op_mbox_info);
	install_custom_opcode(OP_MBOX_SEND_,  (int *)&op_mbox_send);
	install_custom_opcode(OP_MBOX_RECV_,  (int *)&op_mbox_recv);
	install_custom_opcode(OP_MBOX_AWAIT_, (int *)&op_mbox_await);
	install_custom_opcode(OP_MSG_SEND_,   (int *)&op_msg_send);
	install_custom_opcode(OP_MSG_RECV_,   (int *)&op_msg_recv);
	return KXERR_NONE;
}
static int mbox_shutdown () { return KXERR_NONE; }
static int __attribute__((constructor)) mbox_constructor () {
	kext_register("mbox", (int *)&mbox_init, (int *)&mbox_start, (int *)&mbox_shutdown);
}

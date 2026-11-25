// c4bb_mbox.h -- the mailbox, from inside the machine.
//
// A region of RAM the host and the guest share, holding two rings of
// i32 frames: one the host writes and the guest reads (in), one the
// guest writes and the host reads (out). A frame is [len][type][seq]
// followed by len-3 payload words; head and tail are monotonic word
// counters and a frame never wraps -- a -1 marker at the head means
// "skip to the next multiple of cap". Plain loads and stores: no
// opcode, no copy. docs/c4bb-design.md "The mailbox".
//
// BOARD ONLY, and only after the caller has seen C4I_MBOX (0x2000) in
// __c4_info(): on native c4m these addresses are ordinary memory, and
// a program that includes this must say "not fitted" and stop there
// when the bit is absent. c4cc has no #include; pass this file as a
// source ahead of the program, the way u0lite.h is passed.

enum {
	MB_BASE = 0x1b4,       // r: region base (0 = none)
	MB_LEN  = 0x1b8,       // r: region bytes
	MB_BELL = 0x1bc,       // w: doorbell to the host; r: pending host irqs (read-to-clear)
	MB_CAP = 0, MB_HEAD = 1, MB_TAIL = 2, MB_DATA = 3,   // ring header words
	MB_HDR = 3,            // frame header words: len, type, seq
	MB_BELL_REPLY = 1,     // "I wrote frames"
	MB_BELL_IDLE  = 2,     // "my inbox is empty; you may stop me"
	C4I_MBOX = 0x2000
};

int *mb_in;   // host -> guest ring (we read)
int *mb_out;  // guest -> host ring (we write)
int  mb_seq;  // our next outgoing sequence number

// 1 when the mailbox is fitted and the rings are mapped, else 0.
int mb_init () {
	int base, len, half;
	if (!(__c4_info() & C4I_MBOX)) return 0;
	base = *(int *)MB_BASE;
	len  = *(int *)MB_LEN;
	if (!base || !len) return 0;
	half = len / 2;
	mb_in  = (int *)base;
	mb_out = (int *)(base + half);
	mb_seq = 0;
	return 1;
}

// The next frame in the inbox, or 0. The pointer is valid until mb_done.
int *mb_poll () {
	int cap, head, tail, at;
	cap = mb_in[MB_CAP]; head = mb_in[MB_HEAD]; tail = mb_in[MB_TAIL];
	if (head == tail) return 0;
	at = tail % cap;
	if (mb_in[MB_DATA + at] == -1) {           // skip marker: producer wrapped
		tail = tail + (cap - at);
		mb_in[MB_TAIL] = tail;
		if (head == tail) return 0;
		at = 0;
	}
	return mb_in + MB_DATA + at;
}

// Consume the frame mb_poll returned.
void mb_done (int *frame) {
	mb_in[MB_TAIL] = mb_in[MB_TAIL] + frame[0];
}

// Write a frame of `n` payload words to the outbox. 1 on success, 0 if full.
int mb_send (int type, int *payload, int n) {
	int cap, head, tail, at, len, free, i, *f;
	cap = mb_out[MB_CAP]; head = mb_out[MB_HEAD]; tail = mb_out[MB_TAIL];
	len = MB_HDR + n;
	free = cap - (head - tail);
	at = head % cap;
	if (at + len > cap) {                      // no room before the end: wrap
		if (free < (cap - at) + len) return 0;
		mb_out[MB_DATA + at] = -1;
		head = head + (cap - at);
		at = 0;
		free = cap - (head - tail);
	}
	if (free < len) return 0;
	f = mb_out + MB_DATA + at;
	f[1] = type; f[2] = mb_seq; mb_seq = mb_seq + 1;
	i = 0;
	while (i < n) { f[MB_HDR + i] = payload[i]; i = i + 1; }
	f[0] = len;                                // length last: the frame is whole when it is visible
	mb_out[MB_HEAD] = head + len;
	return 1;
}

void mb_bell (int v) { *(int *)MB_BELL = v; }
int  mb_irqs ()      { return *(int *)MB_BELL; }

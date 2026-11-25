// c4bb_info.h -- is this actually a breadboard?
//
// Separate from c4bb.h on purpose. Everything in that header is a plain
// load or store and costs no opcode above EXIT, which is what lets a
// C4DOS-rung program read the clock and arm the timer. This one asks
// c4_info(), and INFO is opcode 57 -- so including it puts the program
// above base c4, and c4cc compiles every function in a header whether
// it is called or not.
//
// Who needs it: something that ALSO runs somewhere else. A C4DOS
// transient only ever runs here and can assume the registers exist; a
// kernel that runs under native c4m as well cannot, because there is no
// device window at 0x19c there and the read would be a wild access
// rather than a zero. The capability is announced, never probed.
//
// See docs/c4bb-storage.md.

enum { BB_I_PIT = 0x800 };      // c4bb has RTC_MS and PIT_MS

int bb_has_clock () { return __c4_info() & BB_I_PIT; }

// c4bb can be asked which PC last wrote to an address
// (src/c4bb/sim/arena-whowrote.js, cli.js --whowrote). Fitted only when
// the machine was started with that flag, which is why it is announced
// here rather than assumed: the port lives at 0x1a4/0x1a8, and under
// native c4m those addresses are c4m's OWN MEMORY. Writing there to see
// what happens is the one thing this must never do.
enum { BB_I_WHOWROTE = 0x1000 };
enum { BB_WW_QUERY = 420, BB_WW_ANSWER = 424 };   // 0x1a4, 0x1a8

int bb_has_whowrote () { return __c4_info() & BB_I_WHOWROTE; }

// Which PC last stored to `addr`, or 0 for "nobody yet" / not fitted.
// CHECK bb_has_whowrote() FIRST.
int bb_whowrote (int addr) {
	int *q, *ans;
	q = (int *)BB_WW_QUERY;
	ans = (int *)BB_WW_ANSWER;
	*q = addr;
	return *ans;
}

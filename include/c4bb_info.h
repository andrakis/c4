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

// c4or1k stdin polling -- the exact non-blocking-fd pattern
// src/c4ix/con.c uses, for the same reason: a blocking read(0, ...)
// would freeze this whole single-threaded VM process waiting for a
// keystroke that may never come, taking every other guest instruction
// down with it. See con.c and run-c4or1k.sh (which puts the *terminal*
// in raw mode -- a separate, host-side concern from this file's "don't
// block the VM on a read" concern).

void con_init();

// Polls stdin (rate-limited, see con.c), feeding any bytes found
// straight into uart_receive_char(). Call this periodically from the
// main step loop -- once per instruction batch, not once per guest
// instruction, or the poll's own host read() call dominates runtime.
void con_poll_and_feed();

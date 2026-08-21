// c4or1k stdin polling -- the exact non-blocking-fd pattern
// src/c4ix/console.c uses, for the same reason: a blocking read(0, ...)
// would freeze this whole single-threaded VM process waiting for a
// keystroke that may never come, taking every other guest instruction
// down with it. See console.c and run-c4or1k.sh (which puts the *terminal*
// in raw mode -- a separate, host-side concern from this file's "don't
// block the VM on a read" concern).

void con_init();

// M18: put fd 0 in raw mode (via __c4_termraw -- native termios or the
// c4mp TRAW opcode) so a typed Ctrl+C is forwarded to the guest as a
// byte instead of killing the emulator. Enabled by main.c in boot mode
// only (the m1/m2/m3 checks run under c4m, which has no TRAW opcode).
// tty-aware: a no-op returning "not raw" when fd 0 isn't a tty, so the
// escape handling below stays off for piped input. con_raw_disable
// restores the terminal; also done on exit.
void con_raw_enable();
void con_raw_disable();

// Polls stdin (rate-limited, see console.c), feeding any bytes found
// straight into uart_receive_char(). Call this periodically from the
// main step loop -- once per instruction batch, not once per guest
// instruction, or the poll's own host read() call dominates runtime.
void con_poll_and_feed();

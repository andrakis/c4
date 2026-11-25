#include "console.h"
#include "uart.h"

// O_NONBLOCK is 0x800 on Linux; no header defines it for this dialect
// (same constant src/c4ix/console.c uses, for the same reason).
enum { CON_O_RDONLY = 0, CON_O_NONBLOCK = 0x800 };
enum { CON_BUF = 256 };
enum { CON_POLL_CYCLES = 100000 }; // matches c4ix/console.c's gate interval

int con_fd;
int con_eof;
int con_gate;
int con_raw;      // 1 once fd 0 is actually in raw mode (a real tty)
int con_esc;      // 1 if the previous byte was the Ctrl-] escape prefix
char con_buf[CON_BUF];

// Ctrl-] (telnet's escape) chosen over Ctrl-A so the guest shell keeps
// Ctrl-A for beginning-of-line. Ctrl-] x quits the emulator; Ctrl-] Ctrl-]
// sends a literal Ctrl-]. Only active when raw mode engaged (interactive).
enum { CON_ESC = 0x1D };

void con_raw_enable() { con_raw = __c4_termraw(1); }
void con_raw_disable() { if (con_raw) { __c4_termraw(0); con_raw = 0; } }

void con_init() {
    con_fd = open("/dev/stdin", CON_O_RDONLY | CON_O_NONBLOCK);
    con_eof = 0;
    con_gate = 0;
}

void con_poll_and_feed() {
    int now, n, i;

    if (con_fd < 0 || con_eof) return;

    now = __c4_cycles();
    if (con_gate && now - con_gate < CON_POLL_CYCLES) return;
    con_gate = now ? now : 1; // 0 means "gate open"; never store it

    n = read(con_fd, con_buf, CON_BUF);
    if (n < 0) return;   // nothing available yet
    if (n == 0) { con_eof = 1; return; }

    i = 0;
    while (i < n) {
        int c;
        c = con_buf[i] & 0xFF;
        ++i;
        // Escape handling only when interactive (raw). For piped input
        // (tests) bytes pass through untouched, so exact-echo tests and
        // the m-checks are unaffected.
        if (con_raw) {
            if (con_esc) {
                con_esc = 0;
                if (c == 'x' || c == 'X') {            // Ctrl-] x : quit
                    con_raw_disable();
                    printf("\r\nc4or1k: exit (Ctrl-] x)\r\n");
                    exit(0);
                }
                if (c == CON_ESC) { uart_receive_char(CON_ESC); continue; } // literal Ctrl-]
                uart_receive_char(CON_ESC);            // prefix wasn't a command:
                uart_receive_char(c);                  // deliver it, then the byte
                continue;
            }
            if (c == CON_ESC) { con_esc = 1; continue; }
        }
        uart_receive_char(c);   // everything else, incl. Ctrl-C (0x03), goes to the guest
    }
}

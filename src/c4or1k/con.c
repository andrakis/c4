#include "con.h"
#include "uart.h"

// O_NONBLOCK is 0x800 on Linux; no header defines it for this dialect
// (same constant src/c4ix/con.c uses, for the same reason).
enum { CON_O_RDONLY = 0, CON_O_NONBLOCK = 0x800 };
enum { CON_BUF = 256 };
enum { CON_POLL_CYCLES = 100000 }; // matches c4ix/con.c's gate interval

int con_fd;
int con_eof;
int con_gate;
char con_buf[CON_BUF];

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
    while (i < n) { uart_receive_char(con_buf[i]); i = i + 1; }
}

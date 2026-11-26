// c4ix_net.h -- C4IX's sockets for programs (docs/c4pc-network.md): the
// kernel's net.c through its syscalls, and addresses as text. ping,
// ifconfig and nc include it. An address is its 32 bits in an int, the
// first byte highest (10.4.0.2 is 0x0a040002); on the 64-bit word the
// high half is 0.

#ifndef __C4IX_NET_H
#define __C4IX_NET_H 1

enum {
    SYS_SOCKET = 258, SYS_BIND = 259, SYS_SENDTO = 260, SYS_RECVFROM = 261, SYS_NETCTL = 262,
    SOCK_UDP = 1, SOCK_ICMP = 2,
    NET_TIMEDOUT = -2, NET_NOWAIT = -1,
    // NETCTL 0's words: the card, its MAC's six bytes, address, mask, frames
    // and bytes in and out, dropped, round inside, malformed, to no port,
    // sockets open
    NI_UP = 0, NI_MAC = 1, NI_IP = 7, NI_MASK = 8, NI_RX = 9, NI_RXB = 10, NI_TX = 11, NI_TXB = 12,
    NI_DROP = 13, NI_LO = 14, NI_BAD = 15, NI_NOPORT = 16, NI_SOCKS = 17, NI_WORDS = 18
};
#define NET_ALL 0xffffffff

static int nsys(int num, int a, int b, int c) { return __c4_opcode(c, b, a, num); }
// a socket: SOCK_UDP or SOCK_ICMP (echo); its fd, or -1
static int usocket(int kind) { return nsys(SYS_SOCKET, kind, 0, 0); }
// its port (UDP) or ident (ICMP): PORT, or one chosen (0); the port, or -1
static int ubind(int fd, int port) { return nsys(SYS_BIND, fd, port, 0); }
// LEN bytes at BUF to IP:PORT (ICMP: a message, its checksum filled in); bytes sent, or -1
static int usendto(int fd, char *buf, int len, int ip, int port) {
    int w[4];
    w[0] = (int)buf; w[1] = len; w[2] = ip; w[3] = port;
    return nsys(SYS_SENDTO, fd, (int)w, 0);
}
// the next datagram into BUF (LEN at most), from *IP:*PORT (ICMP: *PORT
// its TTL): its length; NET_TIMEDOUT when DEADLINE (the machine's ms, as
// __time() says) passed first. DEADLINE 0 waits as long as it takes,
// NET_NOWAIT not at all
static int urecvfrom(int fd, char *buf, int len, int *ip, int *port, int deadline) {
    int w[5], r;
    w[0] = (int)buf; w[1] = len; w[2] = 0; w[3] = 0; w[4] = deadline;
    r = nsys(SYS_RECVFROM, fd, (int)w, 0);
    if (ip) *ip = w[2] & NET_ALL;
    if (port) *port = w[3];
    return r;
}
static int unetinfo(int *v) { return nsys(SYS_NETCTL, 0, (int)v, NI_WORDS); }
static int unetset(int ip, int bits) { return nsys(SYS_NETCTL, 1, ip, bits); }

// "a.b.c.d" to an address, 0 if it is not one; *BITS, if not 0, a "/N" after it (-1 none)
static int uparseip(char *s, int *bits) {
    int a, k, v, n;
    a = 0; k = 0;
    if (bits) *bits = 0 - 1;
    while (k < 4) {
        if (*s < '0' || *s > '9') return 0;
        v = 0; n = 0;
        while (*s >= '0' && *s <= '9' && n < 4) { v = v * 10 + (*s - '0'); ++s; ++n; }
        if (v > 255) return 0;
        a = (a << 8) | v;
        ++k;
        if (k < 4) { if (*s != '.') return 0; ++s; }
    }
    if (*s == '/' && bits) { ++s; *bits = 0; while (*s >= '0' && *s <= '9') { *bits = *bits * 10 + (*s - '0'); ++s; } }
    if (*s) return 0;
    return a & NET_ALL;
}
// HOST's address: a dotted one, localhost, or a machine's name on the
// hypervisor's switch (its name, vmN, or control: the switch knows them); 0 none
static int uresolve(char *s) {
    int a;
    if ((a = uparseip(s, 0))) return a;
    if (!ustrcmp(s, "localhost")) return 2130706433;
    return nsys(SYS_NETCTL, 2, (int)s, 0) & NET_ALL;
}
// A as "a.b.c.d" into OUT (16 bytes)
static char *uipstr(int a, char *out) {
    char *p;
    int k, v;
    p = out;
    k = 24;
    while (k >= 0) {
        v = (a >> k) & 255;
        if (v >= 100) *p++ = '0' + v / 100;
        if (v >= 10) *p++ = '0' + v / 10 % 10;
        *p++ = '0' + v % 10;
        if (k) *p++ = '.';
        k = k - 8;
    }
    *p = 0;
    return out;
}

#endif

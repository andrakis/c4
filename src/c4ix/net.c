// net.c -- C4IX's network (docs/c4pc-network.md): the c4net card's
// driver, ARP, IPv4, ICMP echo, UDP, loopback, and sockets as file
// descriptors. TCP is the next milestone.
//
// The card is c4pc's (src/c4pc/net.c): a window of registers at 0x580,
// the kernel's alone, which sends and receives whole Ethernet frames
// between the kernel's memory and the hypervisor's switch, a command each
// (INFO's C4IX_I_NET says it is there). It interrupts nothing: the
// scheduler's trap looks at its queue each millisecond of the machine's
// clock (net_tick, a register read when nothing has come), and a syscall
// that waits for a datagram looks too. The host ends a napping machine's
// nap when a frame comes for it.
//
// What is taken in: ARP (requests for this machine's address answered,
// replies learnt), IPv4 to this machine, its subnet's broadcast or
// 255.255.255.255 -- no options looked at, no fragments (the MTU is 1500,
// and nothing here sends a datagram past it) -- ICMP echo requests
// (answered here) and replies (to ping's sockets), and UDP (to the socket
// bound to its port). 127.0.0.0/8 and the machine's own address go round
// inside, never to the card. One subnet, no gateway: an address off it
// has no route.
//
// A socket is a vnode (VN_SOCK, vn->host its slot here) behind an fd, so
// close() and a task's end free it; its datagrams wait in a queue of 8,
// and a ninth is dropped (counted). RECVFROM blocks as a pipe's reader
// does, on the scheduler's poll queue, until a datagram or its deadline.

#include "c4ix.h"

enum {
    NET_STATUS = 0x580, NET_MAC0 = 0x584, NET_MAC1 = 0x588, NET_IP = 0x58c,
    NET_ADDR = 0x590, NET_LEN = 0x594, NET_CMD = 0x598, NET_RXQ = 0x59c,
    NCMD_SEND = 1, NCMD_RECV = 2, NCMD_RESOLVE = 3,
    FRAME = 1536, MTU = 1500, UDP_MAX = 1472, QD = 1480,
    NSOCK = 16, SQ = 8, NARP = 16,
    ET_IP = 2048, ET_ARP = 2054,
    IP_ICMP = 1, IP_UDP = 17,
    EPHEM = 49152
};
// An address is its 32 bits, in a word: on the 64-bit word the high half
// is 0 (ALL is 2^32 - 1 there, -1 at 32 bits), so every address is masked
// with ALL as it comes in
#define ALL 0xffffffff

int net_up;                     // a card (C4IX_I_NET)
int net_ms;                     // the machine ms the card was last looked at (net_tick)
static int net_lk[2];
static char rxf[FRAME];         // a frame from the card
static char txf[FRAME];         // ... to it: a datagram is built here, at txf + 34
static char lof[FRAME];         // a datagram going round inside (loopback)
static char hold[FRAME];        // ARP's held datagrams are copied out of txf through here
static char mac[6];
static int my_ip, my_mask;
static int ip_id;
static int n_rx, n_rxb, n_tx, n_txb, n_drop, n_lo, n_bad, n_noport, n_echo;

// ARP: an address's MAC, and one datagram held while it is asked for
static int arp_ip[NARP];
static int arp_ok[NARP];
static char arp_mac[NARP * 6];
static char *arp_pend[NARP];
static int arp_plen[NARP];
static int arp_next;

// sockets: kind (0 free, 1 UDP, 2 ICMP echo), port (ICMP: the ident),
// and a queue of SQ datagrams, QD bytes each, from s_qip:s_qport
static int s_kind[NSOCK];
static int s_port[NSOCK];
static char *s_q[NSOCK];
static int s_qlen[NSOCK * SQ];
static int s_qip[NSOCK * SQ];
static int s_qport[NSOCK * SQ];
static int s_head[NSOCK];
static int s_tail[NSOCK];
static int s_drop[NSOCK];
static int s_next = EPHEM;

static int g16(char *p) { return (p[0] & 255) << 8 | (p[1] & 255); }
static int g32(char *p) { return (p[0] & 255) << 24 | (p[1] & 255) << 16 | (p[2] & 255) << 8 | (p[3] & 255); }
static void p16(char *p, int v) { p[0] = v >> 8; p[1] = v; }
static void p32(char *p, int v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }
// the ones' complement sum of N bytes at P, added to SUM, folded to 16 bits
static int csum(char *p, int n, int sum) {
    while (n > 1) {
        sum = sum + ((p[0] & 255) << 8 | (p[1] & 255));
        p = p + 2;
        n = n - 2;
    }
    if (n > 0) sum = sum + ((p[0] & 255) << 8);
    while (sum >> 16) sum = (sum & 65535) + (sum >> 16);
    return sum;
}
static int is_loop(int a) { return ((a >> 24) & 255) == 127; }
static int bcast() { return (my_ip | (my_mask ^ ALL)) & ALL; }
// the address a datagram to DST goes from
static int src_for(int dst) { return is_loop(dst) || !my_ip ? 2130706433 : my_ip; }   // 127.0.0.1

// "a.b.c.d" for the log
static char *ip_str(int a, char *out) {
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

// ---- the card ----
static int card_send(char *f, int len) {
    int r;
    *(int *)NET_ADDR = (int)f;
    *(int *)NET_LEN = len;
    *(int *)NET_CMD = NCMD_SEND;
    r = *(int *)NET_CMD;
    if (r > 0) { ++n_tx; n_txb = n_txb + len; }
    return r;
}

static void ip_in(char *p, int len);

// the Ethernet header at F: to DST's MAC (6 bytes at D), from this card, TYPE
static void eth_hdr(char *f, char *d, int type) {
    memcpy(f, d, 6);
    memcpy(f + 6, mac, 6);
    p16(f + 12, type);
}

// an ARP packet of OP (1 request, 2 reply) to TIP at TMAC (broadcast for a request)
static void arp_send(int op, int tip, char *tmac) {
    char *a;
    char ff[6];
    memset(ff, 255, 6);
    eth_hdr(txf, op == 1 ? ff : tmac, ET_ARP);
    a = txf + 14;
    p16(a, 1); p16(a + 2, ET_IP); a[4] = 6; a[5] = 4; p16(a + 6, op);
    memcpy(a + 8, mac, 6); p32(a + 14, my_ip);
    if (op == 1) memset(a + 18, 0, 6); else memcpy(a + 18, tmac, 6);
    p32(a + 24, tip);
    card_send(txf, 42);
}
static int arp_find(int ip) {
    int k;
    k = 0;
    while (k < NARP) { if (arp_ip[k] == ip && (arp_ok[k] || arp_pend[k])) return k; ++k; }
    return 0 - 1;
}
// IP is at MAC M: kept, and a datagram held for it sent
static void arp_learn(int ip, char *m) {
    int k;
    if ((k = arp_find(ip)) < 0) {
        k = arp_next;
        arp_next = (arp_next + 1) % NARP;
        arp_ip[k] = ip;
        arp_plen[k] = 0;
    }
    memcpy(arp_mac + k * 6, m, 6);
    arp_ok[k] = 1;
    if (arp_plen[k]) {
        eth_hdr(hold, m, ET_IP);
        memcpy(hold + 14, arp_pend[k], arp_plen[k]);
        card_send(hold, 14 + arp_plen[k]);
        arp_plen[k] = 0;
    }
}
static void arp_in(char *a, int len) {
    int op, sip, tip;
    if (len < 28 || g16(a) != 1 || g16(a + 2) != ET_IP || a[4] != 6 || a[5] != 4) return;
    op = g16(a + 6);
    sip = g32(a + 14);
    tip = g32(a + 24);
    if (!my_ip || tip != my_ip) return;
    arp_learn(sip, a + 8);
    if (op == 1) arp_send(2, sip, a + 8);
}

// ---- IPv4 ----

// the datagram at txf + 14, its payload (PLEN bytes) at txf + 34 already,
// of PROTO to DST: round inside, to the card, or held for ARP. PLEN, or -1
// (no route)
static int ip_out(int proto, int dst, int plen) {
    char *h;
    char ff[6];
    int len, k, src;
    h = txf + 14;
    len = 20 + plen;
    src = src_for(dst);
    h[0] = 69; h[1] = 0; p16(h + 2, len); ip_id = (ip_id + 1) & 65535; p16(h + 4, ip_id);
    p16(h + 6, 16384); h[8] = 64; h[9] = proto; p16(h + 10, 0);
    p32(h + 12, src); p32(h + 16, dst);
    p16(h + 10, csum(h, 20, 0) ^ 65535);
    if (is_loop(dst) || (my_ip && dst == my_ip)) {
        memcpy(lof, h, len);
        ++n_lo;
        ip_in(lof, len);
        return plen;
    }
    if (!net_up || !my_ip) return 0 - 1;
    if (dst == ALL || dst == bcast()) {
        memset(ff, 255, 6);
        eth_hdr(txf, ff, ET_IP);
        card_send(txf, 14 + len);
        return plen;
    }
    if ((dst ^ my_ip) & my_mask) return 0 - 1;        // off the subnet: no gateway yet
    k = arp_find(dst);
    if (k >= 0 && arp_ok[k]) {
        eth_hdr(txf, arp_mac + k * 6, ET_IP);
        card_send(txf, 14 + len);
        return plen;
    }
    // held (the last one asked for, if more come first), and asked for
    if (k < 0) {
        k = arp_next;
        arp_next = (arp_next + 1) % NARP;
        arp_ip[k] = dst;
        arp_ok[k] = 0;
    }
    if (!arp_pend[k] && !(arp_pend[k] = malloc(MTU))) return 0 - 1;
    memcpy(arp_pend[k], h, len);
    arp_plen[k] = len;
    arp_send(1, dst, 0);
    return plen;
}

// a datagram for socket S from IP:PORT
static void sock_put(int s, int ip, int port, char *d, int n) {
    int at;
    if (s_head[s] - s_tail[s] >= SQ) { ++s_drop[s]; return; }
    at = s * SQ + s_head[s] % SQ;
    if (n > QD) n = QD;
    memcpy(s_q[s] + (s_head[s] % SQ) * QD, d, n);
    s_qlen[at] = n;
    s_qip[at] = ip;
    s_qport[at] = port;
    s_head[s] = s_head[s] + 1;
}

static void icmp_in(int src, char *m, int n, int ttl) {
    int s, id;
    if (n < 8 || csum(m, n, 0) != 65535) { ++n_bad; return; }
    if ((m[0] & 255) == 8) {
        // an echo request: the same back, as a reply
        if (n > MTU - 20) return;
        memcpy(txf + 34, m, n);
        txf[34] = 0;
        p16(txf + 36, 0);
        p16(txf + 36, csum(txf + 34, n, 0) ^ 65535);
        ++n_echo;
        ip_out(IP_ICMP, src, n);
        return;
    }
    if ((m[0] & 255) != 0) return;
    // a reply: to the ping whose ident it has (or one that took none)
    id = g16(m + 4);
    s = 0;
    while (s < NSOCK) {
        if (s_kind[s] == 2 && (s_port[s] == id || !s_port[s])) sock_put(s, src, ttl, m, n);
        ++s;
    }
}

static int udp_sum(int src, int dst, char *u, int n) {
    int sum;
    sum = (src >> 16 & 65535) + (src & 65535) + (dst >> 16 & 65535) + (dst & 65535) + IP_UDP + n;
    return csum(u, n, sum);
}
static void udp_in(int src, int dst, char *u, int n) {
    int ulen, port, s;
    if (n < 8) return;
    ulen = g16(u + 4);
    if (ulen < 8 || ulen > n) { ++n_bad; return; }
    if (g16(u + 6) && udp_sum(src, dst, u, ulen) != 65535) { ++n_bad; return; }
    port = g16(u + 2);
    s = 0;
    while (s < NSOCK) {
        if (s_kind[s] == 1 && s_port[s] == port) { sock_put(s, src, g16(u), u + 8, ulen - 8); return; }
        ++s;
    }
    ++n_noport;
}

static void ip_in(char *p, int len) {
    int hl, tl, dst, src;
    if (len < 20 || ((p[0] >> 4) & 15) != 4) { ++n_bad; return; }
    hl = (p[0] & 15) * 4;
    tl = g16(p + 2);
    if (hl < 20 || tl > len || tl < hl || csum(p, hl, 0) != 65535) { ++n_bad; return; }
    if (g16(p + 6) & 16383) return;                   // a fragment: not taken
    src = g32(p + 12);
    dst = g32(p + 16);
    if (!is_loop(dst) && dst != my_ip && dst != ALL && dst != bcast()) return;
    if ((p[9] & 255) == IP_ICMP) icmp_in(src, p + hl, tl - hl, p[8] & 255);
    else if ((p[9] & 255) == IP_UDP) udp_in(src, dst, p + hl, tl - hl);
}

// every frame the card has: taken in
static void net_poll() {
    int n, guard;
    if (!net_up) return;
    guard = 0;
    while (guard < 64 && *(int *)NET_RXQ > 0) {
        *(int *)NET_ADDR = (int)rxf;
        *(int *)NET_LEN = FRAME;
        *(int *)NET_CMD = NCMD_RECV;
        n = *(int *)NET_CMD;
        if (n <= 0) break;
        if (n > FRAME) n = FRAME;
        ++n_rx;
        n_rxb = n_rxb + n;
        if (n >= 14 && g16(rxf + 12) == ET_ARP) arp_in(rxf + 14, n - 14);
        else if (n >= 34 && g16(rxf + 12) == ET_IP) ip_in(rxf + 14, n - 14);
        ++guard;
    }
}

// each ms of the machine's clock, from the scheduler's trap (sched.c)
void net_tick(int now) {
    net_ms = now;
    if (*(int *)NET_RXQ <= 0) return;
    klock_take(net_lk);
    net_poll();
    klock_drop(net_lk);
}

static int hexc(int v) { return v < 10 ? '0' + v : 'a' + v - 10; }

void net_init() {
    char m[24], a[20];
    int lo, hi, k;
    net_up = 0;
    my_mask = (0 - 256) & ALL;
    if (!(__c4_info() & C4IX_I_NET) || !(*(int *)NET_STATUS & 1)) return;
    lo = *(int *)NET_MAC0;
    hi = *(int *)NET_MAC1;
    mac[0] = lo; mac[1] = lo >> 8; mac[2] = lo >> 16; mac[3] = lo >> 24; mac[4] = hi; mac[5] = hi >> 8;
    my_ip = *(int *)NET_IP & ALL;
    net_up = 1;
    k = 0;
    while (k < 6) {
        m[k * 3] = hexc((mac[k] >> 4) & 15);
        m[k * 3 + 1] = hexc(mac[k] & 15);
        m[k * 3 + 2] = k < 5 ? ':' : 0;
        ++k;
    }
    klog("net0: c4net, MAC %s, %s/24 (the hypervisor's switch)\n", m, ip_str(my_ip, a));
}

// ---- sockets (sys.c's syscalls) ----

// a socket of KIND (1 UDP, 2 ICMP echo): its slot, or -1
int net_socket(int kind) {
    int s;
    if (kind != 1 && kind != 2) return 0 - 1;
    klock_take(net_lk);
    s = 0;
    while (s < NSOCK && s_kind[s]) ++s;
    if (s == NSOCK || (!s_q[s] && !(s_q[s] = malloc(SQ * QD)))) { klock_drop(net_lk); return 0 - 1; }
    s_kind[s] = kind;
    s_port[s] = 0;
    s_head[s] = 0;
    s_tail[s] = 0;
    s_drop[s] = 0;
    klock_drop(net_lk);
    return s;
}
// its last fd closed (vfs.c vn_release): its slot free again (its queue kept, for the next)
void net_close(int s) {
    if (s >= 0 && s < NSOCK) s_kind[s] = 0;
}
int net_readable(int s) { return s >= 0 && s < NSOCK && s_head[s] != s_tail[s]; }

static int port_used(int kind, int port) {
    int s;
    s = 0;
    while (s < NSOCK) { if (s_kind[s] == kind && s_port[s] == port) return 1; ++s; }
    return 0;
}
// socket S's port (UDP) or ident (ICMP): PORT, or one chosen (0). The port, or -1 (taken)
int net_bind(int s, int port) {
    int k;
    if (s < 0 || s >= NSOCK || !s_kind[s] || port < 0 || port > 65535) return 0 - 1;
    klock_take(net_lk);
    if (port && port_used(s_kind[s], port)) { klock_drop(net_lk); return 0 - 1; }
    k = 0;
    while (!port && k < 16384) {
        if (!port_used(s_kind[s], s_next)) port = s_next;
        s_next = s_next + 1;
        if (s_next > 65535) s_next = EPHEM;
        ++k;
    }
    s_port[s] = port;
    klock_drop(net_lk);
    return port ? port : 0 - 1;
}

// W = {buf, len, ip, port}: a UDP datagram, or an ICMP message (its checksum
// filled in here). The bytes sent, or -1
int net_sendto(int s, int *w) {
    int len, r, dst;
    char *u;
    if (s < 0 || s >= NSOCK || !s_kind[s]) return 0 - 1;
    len = w[1];
    dst = w[2] & ALL;
    if (!dst) return 0 - 1;
    if (s_kind[s] == 1 && !s_port[s] && net_bind(s, 0) < 0) return 0 - 1;
    klock_take(net_lk);
    if (s_kind[s] == 1) {
        if (len < 0 || len > UDP_MAX) { klock_drop(net_lk); return 0 - 1; }
        u = txf + 34;
        p16(u, s_port[s]); p16(u + 2, w[3]); p16(u + 4, len + 8); p16(u + 6, 0);
        memcpy(u + 8, (char *)w[0], len);
        r = udp_sum(src_for(dst), dst, u, len + 8) ^ 65535;
        p16(u + 6, r ? r : 65535);
        r = ip_out(IP_UDP, dst, len + 8);
        if (r >= 0) r = len;
    } else {
        if (len < 8 || len > MTU - 20) { klock_drop(net_lk); return 0 - 1; }
        u = txf + 34;
        memcpy(u, (char *)w[0], len);
        p16(u + 2, 0);
        p16(u + 2, csum(u, len, 0) ^ 65535);
        r = ip_out(IP_ICMP, dst, len);
    }
    klock_drop(net_lk);
    return r;
}
// W = {buf, len, ip, port, deadline}: the next datagram into buf, from ip
// and port (an ICMP reply's TTL), its bytes (its whole length, if buf was
// shorter); NET_AGAIN while there is none (sys.c then waits)
int net_recvfrom(int s, int *w) {
    int at, n;
    if (s < 0 || s >= NSOCK || !s_kind[s]) return 0 - 1;
    klock_take(net_lk);
    net_poll();
    if (s_head[s] == s_tail[s]) { klock_drop(net_lk); return NET_AGAIN; }
    at = s * SQ + s_tail[s] % SQ;
    n = s_qlen[at];
    memcpy((char *)w[0], s_q[s] + (s_tail[s] % SQ) * QD, n < w[1] ? n : w[1]);
    w[2] = s_qip[at];
    w[3] = s_qport[at];
    s_tail[s] = s_tail[s] + 1;
    klock_drop(net_lk);
    return n;
}
// read() on a socket: the next datagram's bytes (the caller saw one waiting)
int net_read(int s, char *buf, int len) {
    int w[5], n;
    w[0] = (int)buf; w[1] = len;
    n = net_recvfrom(s, w);
    if (n == NET_AGAIN) return 0;
    return n < len ? n : len;
}

// NETCTL: OP 0 the counts into A (B words: up, MAC 0-5, ip, mask, rx, rx
// bytes, tx, tx bytes, dropped, loopback, bad, no port, echoes answered,
// sockets open); 1 the address A, mask of B bits; 2 the address of the
// machine named at A (the switch knows them), 0 none
static char rname[40];
int net_ctl(int op, int a, int b) {
    int *o, v[18], k, n;
    char *s;
    if (op == 0) {
        o = (int *)a;
        v[0] = net_up;
        k = 0;
        while (k < 6) { v[1 + k] = mac[k] & 255; ++k; }
        v[7] = my_ip; v[8] = my_mask; v[9] = n_rx; v[10] = n_rxb; v[11] = n_tx; v[12] = n_txb;
        n = 0; k = 0;
        while (k < NSOCK) { n = n + s_drop[k]; ++k; }
        v[13] = n_drop + n; v[14] = n_lo; v[15] = n_bad; v[16] = n_noport;
        n = 0; k = 0;
        while (k < NSOCK) { if (s_kind[k]) ++n; ++k; }
        v[17] = n;
        k = 0;
        while (k < b && k < 18) { o[k] = v[k]; ++k; }
        return k;
    }
    if (op == 1) {
        if (b < 8 || b > 30) return 0 - 1;
        klock_take(net_lk);
        my_ip = a & ALL;
        my_mask = (ALL << (32 - b)) & ALL;
        k = 0;
        while (k < NARP) { arp_ok[k] = 0; arp_plen[k] = 0; arp_ip[k] = 0; ++k; }
        klock_drop(net_lk);
        return 0;
    }
    if (op == 2) {
        if (!net_up) return 0;
        s = (char *)a;
        k = 0;
        while (k < 39 && s[k]) { rname[k] = s[k]; ++k; }
        rname[k] = 0;
        klock_take(net_lk);
        *(int *)NET_ADDR = (int)rname;
        *(int *)NET_CMD = NCMD_RESOLVE;
        n = *(int *)NET_CMD;
        klock_drop(net_lk);
        return n == 0 - 1 ? 0 : n & ALL;
    }
    return 0 - 1;
}

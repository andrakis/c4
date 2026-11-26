// net.c -- C4IX's network (docs/c4pc-network.md): the c4net card's
// driver, ARP, IPv4, ICMP echo, UDP, TCP, loopback, and sockets as file
// descriptors.
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
//
// TCP (N3): a stream socket is read and written as a pipe is, so cat,
// sh's redirections and a shell given one as its stdin work over a
// connection. What it does, and leaves out, for machines a switch apart:
//   - the handshake both ways (CONNECT, LISTEN with a backlog, ACCEPT),
//     the MSS option on a SYN, the close both ways (SHUTDOWN, close),
//     TIME_WAIT (2 s), RST for a port nobody listens on;
//   - a window of TB bytes each way: what comes in order is taken, what
//     comes out of order is dropped and the ACK asks again (the switch
//     loses a frame only when a queue of 64 is full);
//   - a retransmission timer (RTO0 doubling to RTO_MAX), go-back-N from
//     the first byte unacknowledged; a shut window probed a byte at a
//     time; a peer silent through TRIES timeouts (SYN_TRIES for the
//     handshake) gets a RST and the socket an error;
//   - no Nagle, no delayed ACKs (an ACK for every segment with data), no
//     urgent data, no window scaling, no RTT estimate (RTO0 is far above
//     a switch's milliseconds).
// A connection's last close sends a FIN (a RST if bytes came that were
// never read); the slot stays the kernel's until the close is done
// (orphan). Timers run in net_tick each TCP_TICK ms while a TCP socket is
// open, card or not (loopback).
//
// Loopback: a datagram to 127/8 or to the machine's own address goes on a
// queue of LOQ, taken in as the net call that sent it ends (net_unlock):
// TCP answers what comes in, and an answer taken in from inside the send
// would come back before the sender had noted what it sent.

#include "c4ix.h"

enum {
    NET_STATUS = 0x580, NET_MAC0 = 0x584, NET_MAC1 = 0x588, NET_IP = 0x58c,
    NET_ADDR = 0x590, NET_LEN = 0x594, NET_CMD = 0x598, NET_RXQ = 0x59c,
    NCMD_SEND = 1, NCMD_RECV = 2, NCMD_RESOLVE = 3,
    FRAME = 1536, MTU = 1500, UDP_MAX = 1472, QD = 1480,
    NSOCK = 32, SQ = 8, NARP = 16, LOQ = 24,
    ET_IP = 2048, ET_ARP = 2054,
    IP_ICMP = 1, IP_TCP = 6, IP_UDP = 17,
    EPHEM = 49152,
    K_UDP = 1, K_ICMP = 2, K_TCP = 3
};
// TCP's states (RFC 793's), flags and sizes
enum {
    T_CLOSED = 0, T_LISTEN = 1, T_SYNSENT = 2, T_SYNRCVD = 3, T_EST = 4, T_FW1 = 5, T_FW2 = 6,
    T_CLOSEWAIT = 7, T_CLOSING = 8, T_LASTACK = 9, T_TIMEWAIT = 10,
    F_FIN = 1, F_SYN = 2, F_RST = 4, F_PSH = 8, F_ACK = 16,
    TB = 8192, MSS = 1460, RTO0 = 200, RTO_MAX = 2000, TRIES = 8, SYN_TRIES = 5,
    TW_MS = 2000, FW2_MS = 10000, TCP_TICK = 10, BACKLOG = 8,
    E_REFUSED = 1, E_TIMEDOUT = 2, E_RESET = 3
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
static int n_tseg, n_trex, n_trst;

// loopback's queue: LOQ datagrams of MTU bytes (ip_out)
static char *loq;
static int loq_len[LOQ];
static int lo_head, lo_tail, lo_busy;

// ARP: an address's MAC, and one datagram held while it is asked for
static int arp_ip[NARP];
static int arp_ok[NARP];
static char arp_mac[NARP * 6];
static char *arp_pend[NARP];
static int arp_plen[NARP];
static int arp_next;

// sockets: kind (0 free, K_UDP, K_ICMP, K_TCP), port (ICMP: the ident;
// TCP: the local port), and for UDP and ICMP a queue of SQ datagrams, QD
// bytes each, from s_qip:s_qport; TCP's state is its tcb
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

// a TCP socket's connection. Sequence numbers are 32 bits, masked with
// ALL, compared by sdiff. The send ring holds txn bytes from txs, the
// first of them sequence number sbase (una, once the SYN is acknowledged);
// nxt is the next to send (back to sbase on a timeout), hinxt the highest
// sent. The receive ring holds rxn bytes from rxs, rnxt the next expected.
struct tcb {
    int state, rip, rport, parent, child, backlog, accno, err, orphan, tried;
    int iss, una, nxt, hinxt, rnxt, swnd, mss, lastwnd;
    int sbase, txs, txn, rxs, rxn;
    int finq, finsent, finever, finseq, finrcvd;
    int rto, rtat, tries, twat;
    char *rx;
    char *tx;
};
static struct tcb *tcb[NSOCK];
int net_tcp;                    // TCP sockets in use: the trap runs net_tick for their timers
static int tcp_ms;              // when the timers last ran
static int acc_no;              // the order connections were made in, for ACCEPT
static int iss_next;

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
// the sum over a UDP or TCP datagram's N bytes at U and its pseudo-header
static int psum(int src, int dst, int proto, char *u, int n) {
    int sum;
    sum = (src >> 16 & 65535) + (src & 65535) + (dst >> 16 & 65535) + (dst & 65535) + proto + n;
    return csum(u, n, sum);
}
// A - B for sequence numbers, as a signed 32-bit difference (on the 64-bit
// word too): negative when A comes before B
static int sdiff(int a, int b) {
    int d;
    d = (a - b) & ALL;
    if (sizeof(int) > 4 && (d >> 31)) d = d - ALL - 1;
    return d;
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
        // round inside: queued, and taken in as the net call that sent it
        // ends (net_unlock)
        if (!loq && !(loq = malloc(LOQ * MTU))) return 0 - 1;
        if (lo_head - lo_tail >= LOQ) { ++n_drop; return plen; }
        memcpy(loq + (lo_head % LOQ) * MTU, h, len);
        loq_len[lo_head % LOQ] = len;
        lo_head = lo_head + 1;
        ++n_lo;
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

// loopback's queue taken in, and what that sends round inside too. Only
// here, as a net call ends: taken in from inside ip_out, TCP's answer came
// back before the sender had noted what it sent (a FIN's ACK taken for
// one of something never sent)
static void lo_drain() {
    int k, n;
    if (lo_busy) return;
    lo_busy = 1;
    while (lo_tail != lo_head) {
        k = lo_tail % LOQ;
        n = loq_len[k];
        memcpy(lof, loq + k * MTU, n);
        lo_tail = lo_tail + 1;
        ip_in(lof, n);
    }
    lo_busy = 0;
}
static void net_unlock() {
    lo_drain();
    klock_drop(net_lk);
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
    sched_netgen();
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
        if (s_kind[s] == K_ICMP && (s_port[s] == id || !s_port[s])) sock_put(s, src, ttl, m, n);
        ++s;
    }
}

static int udp_sum(int src, int dst, char *u, int n) { return psum(src, dst, IP_UDP, u, n); }
static void udp_in(int src, int dst, char *u, int n) {
    int ulen, port, s;
    if (n < 8) return;
    ulen = g16(u + 4);
    if (ulen < 8 || ulen > n) { ++n_bad; return; }
    if (g16(u + 6) && udp_sum(src, dst, u, ulen) != 65535) { ++n_bad; return; }
    port = g16(u + 2);
    s = 0;
    while (s < NSOCK) {
        if (s_kind[s] == K_UDP && s_port[s] == port) { sock_put(s, src, g16(u), u + 8, ulen - 8); return; }
        ++s;
    }
    ++n_noport;
}

// ---- sockets' slots ----

// a slot of KIND (under net_lk): its queue (UDP, ICMP) or tcb (TCP) made
// the first time, kept for the next. The slot, or -1
static int sock_new(int kind) {
    int s;
    struct tcb *c;
    s = 0;
    while (s < NSOCK && s_kind[s]) ++s;
    if (s == NSOCK) return 0 - 1;
    if (kind == K_TCP) {
        if (!tcb[s]) {
            if (!(c = malloc(sizeof(struct tcb) + 2 * TB))) return 0 - 1;
            tcb[s] = c;
        }
        c = tcb[s];
        memset((char *)c, 0, sizeof(struct tcb));
        c->rx = (char *)c + sizeof(struct tcb);
        c->tx = c->rx + TB;
        c->parent = 0 - 1;
        c->rto = RTO0;
        c->mss = MSS;
        c->backlog = 1;
        ++net_tcp;
    } else if (!s_q[s] && !(s_q[s] = malloc(SQ * QD))) return 0 - 1;
    s_kind[s] = kind;
    s_port[s] = 0;
    s_head[s] = 0;
    s_tail[s] = 0;
    s_drop[s] = 0;
    return s;
}
static void sock_free(int s) {
    if (s_kind[s] == K_TCP) { --net_tcp; tcb[s]->state = T_CLOSED; }
    s_kind[s] = 0;
    sched_netgen();
}

// ---- TCP ----

static int tnow(int ms) {
    int t;
    t = __time() + ms;
    return t ? t : 1;
}
// the retransmission timer, if it is not running
static void tcp_arm(struct tcb *c) { if (!c->rtat) c->rtat = tnow(c->rto); }
// the window offered: the receive ring's room
static int tcp_win(struct tcb *c) {
    c->lastwnd = TB - c->rxn;
    return c->lastwnd;
}

// a segment from local port LP to RIP:RP -- SEQ, ACK (with F_ACK), FLAGS,
// window WIN; the MSS option with a SYN; N1 bytes at D1 and N2 at D2 --
// built at txf + 34 and sent. ip_out's answer (-1: no route)
static int tcp_raw(int rip, int lp, int rp, int seq, int ack, int fl, int win, char *d1, int n1, char *d2, int n2) {
    char *g;
    int hl, len;
    g = txf + 34;
    hl = (fl & F_SYN) ? 24 : 20;
    p16(g, lp); p16(g + 2, rp); p32(g + 4, seq); p32(g + 8, (fl & F_ACK) ? ack : 0);
    g[12] = (hl / 4) << 4; g[13] = fl; p16(g + 14, win); p16(g + 16, 0); p16(g + 18, 0);
    if (fl & F_SYN) { g[20] = 2; g[21] = 4; p16(g + 22, MSS); }
    if (n1 > 0) memcpy(g + hl, d1, n1);
    if (n2 > 0) memcpy(g + hl + n1, d2, n2);
    len = hl + n1 + n2;
    p16(g + 16, psum(src_for(rip), rip, IP_TCP, g, len) ^ 65535);
    ++n_tseg;
    if (fl & F_RST) ++n_trst;
    return ip_out(IP_TCP, rip, len);
}
// socket S's segment: FLAGS, SEQ, and N bytes of its send ring from OFF
static int tcp_send(int s, int fl, int seq, int off, int n) {
    struct tcb *c;
    int at, n1;
    c = tcb[s];
    at = (c->txs + off) % TB;
    n1 = n;
    if (at + n1 > TB) n1 = TB - at;
    return tcp_raw(c->rip, s_port[s], c->rport, seq, c->rnxt, fl, tcp_win(c), c->tx + at, n1, c->tx, n - n1);
}
static void tcp_ack(int s) { tcp_send(s, F_ACK, tcb[s]->nxt, 0, 0); }
static void tcp_rst(int s) { tcp_send(s, F_RST | F_ACK, tcb[s]->nxt, 0, 0); }
// the SYN (connecting) or SYN-ACK (accepting); -1: no route
static int tcp_syn(int s) {
    struct tcb *c;
    c = tcb[s];
    if (c->state == T_SYNSENT) return tcp_send(s, F_SYN, c->iss, 0, 0);
    return tcp_send(s, F_SYN | F_ACK, c->iss, 0, 0);
}
// a connection's first sequence number, and the send ring empty after it
static void tcp_start(struct tcb *c) {
    iss_next = iss_next + 64007;
    c->iss = (__time() * 4096 + iss_next) & ALL;
    c->una = c->iss;
    c->nxt = (c->iss + 1) & ALL;
    c->hinxt = c->nxt;
    c->sbase = c->nxt;
}
// the peer's MSS option (536 without one), at most ours
static int tcp_mss(char *g, int hl) {
    int k, m, l;
    m = 536;
    k = 20;
    while (k < hl && g[k]) {
        if (g[k] == 1) l = 1;                                   // a NOP
        else {
            if (k + 1 >= hl || (l = g[k + 1] & 255) < 2) return m > MSS ? MSS : m;
            if (g[k] == 2 && l == 4 && k + 4 <= hl) m = g16(g + k + 2);
        }
        k = k + l;
    }
    if (m > MSS) m = MSS;
    if (m < 64) m = 64;
    return m;
}

// the connection's end: CLOSED, its timers off; an orphan's slot free
static void tcp_closed(int s) {
    struct tcb *c;
    c = tcb[s];
    c->state = T_CLOSED;
    c->rtat = 0;
    c->twat = 0;
    if (c->orphan) sock_free(s);
    sched_netgen();
}

// what the send ring holds, sent as far as the peer's window lets, then
// the FIN once all of it has gone and one is asked for. FORCE: a byte
// even into a shut window (the timer's probe)
static void tcp_output(int s, int force) {
    struct tcb *c;
    int off, left, room, n;
    c = tcb[s];
    if (c->state < T_EST || c->state == T_TIMEWAIT) return;
    while (1) {
        off = sdiff(c->nxt, c->sbase);
        if (off < 0) off = 0;
        left = c->txn - off;
        if (left <= 0) break;
        room = c->swnd - off;
        if (room <= 0) {
            if (!force) break;
            room = 1;
        }
        n = left;
        if (n > room) n = room;
        if (n > c->mss) n = c->mss;
        tcp_send(s, F_ACK | F_PSH, c->nxt, off, n);
        c->nxt = (c->nxt + n) & ALL;
        if (sdiff(c->nxt, c->hinxt) > 0) c->hinxt = c->nxt;
        force = 0;
        tcp_arm(c);
    }
    if (c->finq && !c->finsent && sdiff(c->nxt, c->sbase) >= c->txn) {
        c->finseq = c->nxt;
        tcp_send(s, F_ACK | F_FIN, c->nxt, 0, 0);
        c->nxt = (c->nxt + 1) & ALL;
        if (sdiff(c->nxt, c->hinxt) > 0) c->hinxt = c->nxt;
        c->finsent = 1;
        c->finever = 1;
        if (c->state == T_EST) c->state = T_FW1;
        else if (c->state == T_CLOSEWAIT) c->state = T_LASTACK;
        tcp_arm(c);
    }
    // bytes waiting behind a shut window: the timer probes it
    if (c->txn > sdiff(c->nxt, c->sbase)) tcp_arm(c);
}

// the timer ran out: the SYN again, or everything from the first byte
// not acknowledged; after too many, the connection given up
static void tcp_rexmit(int s) {
    struct tcb *c;
    c = tcb[s];
    c->rtat = 0;
    ++n_trex;
    c->tries = c->tries + 1;
    if (c->tries > (c->state <= T_SYNRCVD ? SYN_TRIES : TRIES)) {
        if (c->state != T_SYNSENT) tcp_rst(s);
        if (c->parent >= 0) { sock_free(s); return; }        // never accepted
        c->err = E_TIMEDOUT;
        tcp_closed(s);
        return;
    }
    c->rto = c->rto * 2;
    if (c->rto > RTO_MAX) c->rto = RTO_MAX;
    if (c->state == T_SYNSENT || c->state == T_SYNRCVD) {
        tcp_syn(s);
        tcp_arm(c);
        return;
    }
    c->nxt = c->sbase;
    if (c->state == T_FW1 || c->state == T_CLOSING || c->state == T_LASTACK) c->finsent = 0;
    tcp_output(s, 1);
}

static void tcp_timers(int now) {
    int s;
    struct tcb *c;
    s = 0;
    while (s < NSOCK) {
        if (s_kind[s] == K_TCP) {
            c = tcb[s];
            if (c->twat && now - c->twat >= 0) { c->twat = 0; tcp_closed(s); }
            else if (c->rtat && now - c->rtat >= 0) tcp_rexmit(s);
        }
        ++s;
    }
}

// the connection LP from RIP:RP, or -1
static int tcp_find(int lp, int rip, int rp) {
    int s;
    struct tcb *c;
    s = 0;
    while (s < NSOCK) {
        if (s_kind[s] == K_TCP && s_port[s] == lp) {
            c = tcb[s];
            if (c->state != T_CLOSED && c->state != T_LISTEN && c->rip == rip && c->rport == rp) return s;
        }
        ++s;
    }
    return 0 - 1;
}
static int tcp_listener(int lp) {
    int s;
    s = 0;
    while (s < NSOCK) {
        if (s_kind[s] == K_TCP && s_port[s] == lp && tcb[s]->state == T_LISTEN) return s;
        ++s;
    }
    return 0 - 1;
}
// listener L's connections made and not yet accepted: the oldest, or -1
static int tcp_pending(int l) {
    int s, best;
    struct tcb *c;
    best = 0 - 1;
    s = 0;
    while (s < NSOCK) {
        if (s_kind[s] == K_TCP) {
            c = tcb[s];
            if (c->parent == l && c->state >= T_EST && (best < 0 || c->accno - tcb[best]->accno < 0)) best = s;
        }
        ++s;
    }
    return best;
}

// a segment for no connection, nobody listening: a RST back (RFC 793's)
static void tcp_refuse(int src, int lp, int rp, int seq, int ack, int fl, int dlen) {
    if (fl & F_ACK) tcp_raw(src, lp, rp, ack, 0, F_RST, 0, 0, 0, 0, 0);
    else tcp_raw(src, lp, rp, 0, (seq + dlen + ((fl & F_SYN) ? 1 : 0) + ((fl & F_FIN) ? 1 : 0)) & ALL,
                 F_RST | F_ACK, 0, 0, 0, 0, 0);
}

// a SYN for listener L from SRC:SP: a connection of its own, SYN_RCVD,
// while L's backlog has room (a SYN past it is dropped: the peer sends again)
static void tcp_child(int l, int src, int sp, int seq, int win, char *g, int hl) {
    struct tcb *c;
    int s, k, n;
    n = 0;
    k = 0;
    while (k < NSOCK) { if (s_kind[k] == K_TCP && tcb[k]->parent == l) ++n; ++k; }
    if (n >= tcb[l]->backlog || (s = sock_new(K_TCP)) < 0) return;
    c = tcb[s];
    s_port[s] = s_port[l];
    c->rip = src;
    c->rport = sp;
    c->parent = l;
    c->child = 1;
    c->rnxt = (seq + 1) & ALL;
    c->swnd = win;
    c->mss = tcp_mss(g, hl);
    tcp_start(c);
    c->state = T_SYNRCVD;
    tcp_syn(s);
    tcp_arm(c);
}

// connecting: the SYN-ACK (or a RST: refused)
static void tcp_synsent(int s, int src, int seq, int ack, int fl, int win, char *g, int hl) {
    struct tcb *c;
    c = tcb[s];
    if ((fl & F_ACK) && ack != ((c->iss + 1) & ALL)) {
        if (!(fl & F_RST)) tcp_raw(src, s_port[s], c->rport, ack, 0, F_RST, 0, 0, 0, 0, 0);
        return;
    }
    if (fl & F_RST) {
        if (fl & F_ACK) { c->err = E_REFUSED; tcp_closed(s); }
        return;
    }
    if (!(fl & F_SYN) || !(fl & F_ACK)) return;            // a simultaneous open: not done
    c->rnxt = (seq + 1) & ALL;
    c->una = ack;
    c->swnd = win;
    c->mss = tcp_mss(g, hl);
    c->state = T_EST;
    c->tries = 0;
    c->rto = RTO0;
    c->rtat = 0;
    tcp_ack(s);
    tcp_output(s, 0);
    sched_netgen();
}

// a TCP segment, N bytes at G, from SRC to DST
static void tcp_in(int src, int dst, char *g, int n) {
    int sp, dp, seq, ack, hl, fl, win, dlen, s, k, m, at, n1, need;
    struct tcb *c;
    char *d;
    if (n < 20 || psum(src, dst, IP_TCP, g, n) != 65535) { ++n_bad; return; }
    hl = ((g[12] >> 4) & 15) * 4;
    if (hl < 20 || hl > n) { ++n_bad; return; }
    sp = g16(g);
    dp = g16(g + 2);
    seq = g32(g + 4) & ALL;
    ack = g32(g + 8) & ALL;
    fl = g[13] & 63;
    win = g16(g + 14);
    d = g + hl;
    dlen = n - hl;
    if ((s = tcp_find(dp, src, sp)) < 0) {
        if (fl & F_RST) return;
        if ((s = tcp_listener(dp)) >= 0 && (fl & F_SYN) && !(fl & F_ACK)) { tcp_child(s, src, sp, seq, win, g, hl); return; }
        tcp_refuse(src, dp, sp, seq, ack, fl, dlen);
        return;
    }
    c = tcb[s];
    if (c->state == T_SYNSENT) { tcp_synsent(s, src, seq, ack, fl, win, g, hl); return; }
    if (fl & F_RST) {
        // in the window (a RST from the past is not taken)
        k = sdiff(seq, c->rnxt);
        if (k < 0 || k > TB) return;
        if (c->parent >= 0) { sock_free(s); return; }      // not accepted yet: gone
        c->err = E_RESET;
        tcp_closed(s);
        return;
    }
    if (fl & F_SYN) {
        // our SYN-ACK lost (the SYN again), or a SYN in a connection: an ACK says where we are
        if (c->state == T_SYNRCVD && seq == ((c->rnxt - 1) & ALL)) tcp_syn(s);
        else tcp_ack(s);
        return;
    }
    if (!(fl & F_ACK)) return;
    if (c->state == T_SYNRCVD) {
        if (sdiff(ack, c->una) <= 0 || sdiff(ack, c->hinxt) > 0) { tcp_raw(src, dp, sp, ack, 0, F_RST, 0, 0, 0, 0, 0); return; }
        c->state = T_EST;
        acc_no = acc_no + 1;
        c->accno = acc_no;
        sched_netgen();
    }
    // the ACK: what it acknowledges leaves the send ring
    if (sdiff(ack, c->hinxt) > 0) { tcp_ack(s); return; }
    if (sdiff(ack, c->una) >= 0) {
        c->tries = 0;
        c->swnd = win;
        if (sdiff(ack, c->una) > 0) {
            k = sdiff(ack, c->sbase);
            if (k > c->txn) k = c->txn;
            if (k > 0) {
                c->txs = (c->txs + k) % TB;
                c->txn = c->txn - k;
                c->sbase = (c->sbase + k) & ALL;
            }
            c->una = ack;
            if (sdiff(ack, c->nxt) > 0) c->nxt = ack;
            c->rto = RTO0;
            c->rtat = 0;
            if (c->una != c->hinxt) tcp_arm(c);
            sched_netgen();
            if (c->finever && sdiff(ack, c->finseq) > 0) {
                // our FIN acknowledged
                c->finsent = 1;
                if (c->state == T_FW1) { c->state = T_FW2; if (c->orphan) c->twat = tnow(FW2_MS); }
                else if (c->state == T_CLOSING) { c->state = T_TIMEWAIT; c->twat = tnow(TW_MS); }
                else if (c->state == T_LASTACK) { tcp_closed(s); return; }
            }
        }
    }
    // the bytes, if they are the next expected (a part already had is
    // cut off), as many as the ring has room for; then the FIN after them
    need = 0;
    if (dlen > 0 || (fl & F_FIN)) {
        need = 1;
        k = sdiff(c->rnxt, seq);
        if (k >= 0 && k <= dlen && !c->finrcvd) {
            d = d + k;
            dlen = dlen - k;
            m = 0;
            if (dlen > 0 && (c->state == T_EST || c->state == T_FW1 || c->state == T_FW2)) {
                m = TB - c->rxn;
                if (m > dlen) m = dlen;
                at = (c->rxs + c->rxn) % TB;
                n1 = m;
                if (at + n1 > TB) n1 = TB - at;
                memcpy(c->rx + at, d, n1);
                if (m > n1) memcpy(c->rx, d + n1, m - n1);
                c->rxn = c->rxn + m;
                c->rnxt = (c->rnxt + m) & ALL;
                if (m) sched_netgen();
            }
            if ((fl & F_FIN) && m == dlen) {
                c->rnxt = (c->rnxt + 1) & ALL;
                c->finrcvd = 1;
                sched_netgen();
                if (c->state == T_EST) c->state = T_CLOSEWAIT;
                else if (c->state == T_FW1) c->state = T_CLOSING;
                else if (c->state == T_FW2) { c->state = T_TIMEWAIT; c->twat = tnow(TW_MS); }
            }
        }
    }
    if (need) tcp_ack(s);
    tcp_output(s, 0);
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
    else if ((p[9] & 255) == IP_TCP && dst != ALL && (dst == my_ip || is_loop(dst))) tcp_in(src, dst, p + hl, tl - hl);
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
// (with a card, or with TCP sockets open): the card's frames, and TCP's
// timers each TCP_TICK ms
void net_tick(int now) {
    int rx, tm;
    net_ms = now;
    rx = net_up && *(int *)NET_RXQ > 0;
    tm = net_tcp && now - tcp_ms >= TCP_TICK;
    if (!rx && !tm) return;
    klock_take(net_lk);
    if (rx) net_poll();
    if (tm) { tcp_ms = now; tcp_timers(now); }
    net_unlock();
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

// a socket of KIND (1 UDP, 2 ICMP echo, 3 TCP): its slot, or -1
int net_socket(int kind) {
    int s;
    if (kind != K_UDP && kind != K_ICMP && kind != K_TCP) return 0 - 1;
    klock_take(net_lk);
    s = sock_new(kind);
    net_unlock();
    return s;
}
static int is_tcp(int s) { return s >= 0 && s < NSOCK && s_kind[s] == K_TCP; }

// its last fd closed (vfs.c vn_release): its slot free again (its queue
// kept, for the next). A TCP connection is closed first: a listener's
// connections not accepted get a RST; one with bytes never read a RST; a
// connection open its FIN, the slot the kernel's till the close is done
void net_close(int s) {
    struct tcb *c;
    int k;
    if (s < 0 || s >= NSOCK || !s_kind[s]) return;
    klock_take(net_lk);
    if (s_kind[s] != K_TCP) { s_kind[s] = 0; net_unlock(); return; }
    c = tcb[s];
    if (c->state == T_LISTEN) {
        k = 0;
        while (k < NSOCK) {
            if (s_kind[k] == K_TCP && tcb[k]->parent == s) { tcp_rst(k); sock_free(k); }
            ++k;
        }
        sock_free(s);
    } else if (c->state == T_CLOSED || c->state == T_SYNSENT) sock_free(s);
    else if (c->state == T_SYNRCVD || c->rxn > 0) { tcp_rst(s); sock_free(s); }
    else {
        c->orphan = 1;
        c->finq = 1;
        tcp_output(s, 0);
        if (c->state == T_FW2) c->twat = tnow(FW2_MS);
    }
    net_unlock();
}
// a connection's bytes are all read and no more will come
static int tcp_eof(struct tcb *c) { return c->finrcvd || c->state == T_CLOSED; }
int net_readable(int s) {
    struct tcb *c;
    if (s < 0 || s >= NSOCK) return 0;
    if (s_kind[s] != K_TCP) return s_head[s] != s_tail[s];
    c = tcb[s];
    if (c->state == T_LISTEN) return tcp_pending(s) >= 0;
    if (c->state == T_SYNSENT || c->state == T_SYNRCVD) return 0;
    return c->rxn > 0 || tcp_eof(c);
}
// AVAIL: bytes waiting (UDP: the next datagram's), 0 none, -1 a
// connection's end (TCP); a listener 1 with a connection to accept
int net_avail(int s) {
    struct tcb *c;
    if (s < 0 || s >= NSOCK || !s_kind[s]) return 0 - 1;
    if (s_kind[s] != K_TCP) return s_head[s] != s_tail[s] ? s_qlen[s * SQ + s_tail[s] % SQ] : 0;
    c = tcb[s];
    if (c->state == T_LISTEN) return tcp_pending(s) >= 0;
    if (c->state == T_SYNSENT || c->state == T_SYNRCVD) return 0;
    if (c->rxn > 0) return c->rxn;
    return tcp_eof(c) ? 0 - 1 : 0;
}

// PORT taken among KIND's sockets: by any of them (ALL, choosing one), or
// (binding) for TCP by a listener or a socket not a listener's
// connection and not in TIME_WAIT
static int port_used(int kind, int port, int any) {
    int s;
    struct tcb *c;
    s = 0;
    while (s < NSOCK) {
        if (s_kind[s] == kind && s_port[s] == port) {
            if (kind != K_TCP || any) return 1;
            c = tcb[s];
            if (!c->child && c->state != T_TIMEWAIT) return 1;
        }
        ++s;
    }
    return 0;
}
static int bind_locked(int s, int port) {
    int k;
    if (port && port_used(s_kind[s], port, 0)) return 0 - 1;
    k = 0;
    while (!port && k < 16384) {
        if (!port_used(s_kind[s], s_next, 1)) port = s_next;
        s_next = s_next + 1;
        if (s_next > 65535) s_next = EPHEM;
        ++k;
    }
    s_port[s] = port;
    return port ? port : 0 - 1;
}
// socket S's port (UDP, TCP) or ident (ICMP): PORT, or one chosen (0). The port, or -1 (taken)
int net_bind(int s, int port) {
    int r;
    if (s < 0 || s >= NSOCK || !s_kind[s] || port < 0 || port > 65535) return 0 - 1;
    klock_take(net_lk);
    r = bind_locked(s, port);
    net_unlock();
    return r;
}

// CONNECT: the SYN sent to IP:PORT, then NET_AGAIN while it is answered
// (sys.c asks again each ms); 0 connected; -1 no route or a connection
// reset, NET_REFUSED (a RST: nobody listens there), NET_TIMEDOUT
int net_connect(int s, int ip, int port) {
    struct tcb *c;
    int e;
    if (!is_tcp(s)) return 0 - 1;
    klock_take(net_lk);
    c = tcb[s];
    if (c->state == T_CLOSED && c->tried) {
        e = c->err;
        c->err = 0;
        c->tried = 0;
        net_unlock();
        return e == E_REFUSED ? NET_REFUSED : e == E_TIMEDOUT ? NET_TIMEDOUT : e ? 0 - 1 : 0;
    }
    if (c->state == T_CLOSED) {
        ip = ip & ALL;
        if (!ip || port <= 0 || port > 65535 || (!s_port[s] && bind_locked(s, 0) < 0)) { net_unlock(); return 0 - 1; }
        c->rip = ip;
        c->rport = port;
        tcp_start(c);
        c->state = T_SYNSENT;
        c->tried = 1;
        c->rto = RTO0;
        c->tries = 0;
        if (tcp_syn(s) < 0) { c->state = T_CLOSED; c->tried = 0; net_unlock(); return 0 - 1; }
        tcp_arm(c);
        net_unlock();
        return NET_AGAIN;
    }
    e = c->state == T_SYNSENT ? NET_AGAIN : c->state == T_LISTEN ? 0 - 1 : 0;
    if (!e) c->tried = 0;
    net_unlock();
    return e;
}
// LISTEN: connections to its port taken, BACKLOG of them waiting at most
int net_listen(int s, int backlog) {
    struct tcb *c;
    if (!is_tcp(s)) return 0 - 1;
    klock_take(net_lk);
    c = tcb[s];
    if (c->state != T_CLOSED || (!s_port[s] && bind_locked(s, 0) < 0)) { net_unlock(); return 0 - 1; }
    c->state = T_LISTEN;
    c->backlog = backlog < 1 ? 1 : backlog > BACKLOG ? BACKLOG : backlog;
    net_unlock();
    return 0;
}
// ACCEPT: the oldest connection made to listener S, its slot (W, if not 0,
// = {ip, port} its peer); NET_AGAIN none yet
int net_accept(int s, int *w) {
    int k;
    if (!is_tcp(s)) return 0 - 1;
    klock_take(net_lk);
    if (tcb[s]->state != T_LISTEN) { net_unlock(); return 0 - 1; }
    if ((k = tcp_pending(s)) < 0) { net_unlock(); return NET_AGAIN; }
    tcb[k]->parent = 0 - 1;
    if (w) { w[0] = tcb[k]->rip; w[1] = tcb[k]->rport; }
    net_unlock();
    return k;
}
// SHUTDOWN: no more to send -- the FIN, after what the ring holds
int net_shutdown(int s) {
    struct tcb *c;
    if (!is_tcp(s)) return 0 - 1;
    klock_take(net_lk);
    c = tcb[s];
    if (c->state != T_EST && c->state != T_CLOSEWAIT) { net_unlock(); return 0 - 1; }
    c->finq = 1;
    tcp_output(s, 0);
    net_unlock();
    return 0;
}
// write() on a connection: LEN bytes into the send ring, and sent. All of
// them once there is room for all (LEN past the ring: a ring's worth, once
// it is empty); NET_AGAIN until then (sys.c waits a ms at a time); -1 when
// the connection can send no more
int net_write(int s, char *buf, int len) {
    struct tcb *c;
    int n, at, n1;
    if (!is_tcp(s)) return 0 - 1;
    klock_take(net_lk);
    c = tcb[s];
    if ((c->state != T_EST && c->state != T_CLOSEWAIT) || c->finq) { net_unlock(); return 0 - 1; }
    if (len <= 0) { net_unlock(); return 0; }
    if (TB - c->txn < (len < TB ? len : TB)) { net_unlock(); return NET_AGAIN; }
    n = TB - c->txn;
    if (n > len) n = len;
    at = (c->txs + c->txn) % TB;
    n1 = n;
    if (at + n1 > TB) n1 = TB - at;
    memcpy(c->tx + at, buf, n1);
    if (n > n1) memcpy(c->tx, buf + n1, n - n1);
    c->txn = c->txn + n;
    tcp_output(s, 0);
    net_unlock();
    return n;
}

// W = {buf, len, ip, port}: a UDP datagram, or an ICMP message (its checksum
// filled in here). The bytes sent, or -1
int net_sendto(int s, int *w) {
    int len, r, dst;
    char *u;
    if (s < 0 || s >= NSOCK || !s_kind[s] || s_kind[s] == K_TCP) return 0 - 1;
    len = w[1];
    dst = w[2] & ALL;
    if (!dst) return 0 - 1;
    if (s_kind[s] == 1 && !s_port[s] && net_bind(s, 0) < 0) return 0 - 1;
    klock_take(net_lk);
    if (s_kind[s] == 1) {
        if (len < 0 || len > UDP_MAX) { net_unlock(); return 0 - 1; }
        u = txf + 34;
        p16(u, s_port[s]); p16(u + 2, w[3]); p16(u + 4, len + 8); p16(u + 6, 0);
        memcpy(u + 8, (char *)w[0], len);
        r = udp_sum(src_for(dst), dst, u, len + 8) ^ 65535;
        p16(u + 6, r ? r : 65535);
        r = ip_out(IP_UDP, dst, len + 8);
        if (r >= 0) r = len;
    } else {
        if (len < 8 || len > MTU - 20) { net_unlock(); return 0 - 1; }
        u = txf + 34;
        memcpy(u, (char *)w[0], len);
        p16(u + 2, 0);
        p16(u + 2, csum(u, len, 0) ^ 65535);
        r = ip_out(IP_ICMP, dst, len);
    }
    net_unlock();
    return r;
}
// W = {buf, len, ip, port, deadline}: the next datagram into buf, from ip
// and port (an ICMP reply's TTL), its bytes (its whole length, if buf was
// shorter); NET_AGAIN while there is none (sys.c then waits)
int net_recvfrom(int s, int *w) {
    int at, n;
    if (s < 0 || s >= NSOCK || !s_kind[s] || s_kind[s] == K_TCP) return 0 - 1;
    klock_take(net_lk);
    net_poll();
    if (s_head[s] == s_tail[s]) { net_unlock(); return NET_AGAIN; }
    at = s * SQ + s_tail[s] % SQ;
    n = s_qlen[at];
    memcpy((char *)w[0], s_q[s] + (s_tail[s] % SQ) * QD, n < w[1] ? n : w[1]);
    w[2] = s_qip[at];
    w[3] = s_qport[at];
    s_tail[s] = s_tail[s] + 1;
    net_unlock();
    return n;
}
// read() on a connection: what the receive ring holds, LEN at most (0 at
// its end); the window offered again once half the ring is free
static int tcp_read(int s, char *buf, int len) {
    struct tcb *c;
    int m, n1;
    klock_take(net_lk);
    c = tcb[s];
    m = c->rxn;
    if (m > len) m = len;
    if (m > 0) {
        n1 = m;
        if (c->rxs + n1 > TB) n1 = TB - c->rxs;
        memcpy(buf, c->rx + c->rxs, n1);
        if (m > n1) memcpy(buf + n1, c->rx, m - n1);
        c->rxs = (c->rxs + m) % TB;
        c->rxn = c->rxn - m;
        if (!c->finrcvd && c->state >= T_EST && c->state != T_TIMEWAIT && c->lastwnd < TB / 2 && TB - c->rxn >= TB / 2)
            tcp_ack(s);
    }
    net_unlock();
    return m;
}
// read() on a socket: a connection's bytes, or the next datagram's (the caller saw one waiting)
int net_read(int s, char *buf, int len) {
    int w[5], n;
    if (is_tcp(s)) return tcp_read(s, buf, len);
    w[0] = (int)buf; w[1] = len;
    n = net_recvfrom(s, w);
    if (n == NET_AGAIN) return 0;
    return n < len ? n : len;
}

// NETCTL: OP 0 the counts into A (B words: up, MAC 0-5, ip, mask, rx, rx
// bytes, tx, tx bytes, dropped, loopback, bad, no port, echoes answered,
// sockets open, TCP segments sent, TCP timeouts, RSTs sent); 1 the
// address A, mask of B bits; 2 the address of the machine named at A (the
// switch knows them), 0 none; 3 the sockets into A (B words at most), 8
// words each -- kind, TCP state, local port, peer's address and port,
// bytes (datagrams) waiting in, bytes waiting to go, the kernel's
// (closed, finishing) -- and how many
static char rname[40];
int net_ctl(int op, int a, int b) {
    int *o, v[21], k, n;
    char *s;
    struct tcb *c;
    if (op == 3) {
        o = (int *)a;
        n = 0;
        k = 0;
        klock_take(net_lk);
        while (k < NSOCK && (n + 1) * 8 <= b) {
            if (s_kind[k]) {
                o[0] = s_kind[k]; o[2] = s_port[k];
                if (s_kind[k] == K_TCP) {
                    c = tcb[k];
                    o[1] = c->state; o[3] = c->rip; o[4] = c->rport; o[5] = c->rxn; o[6] = c->txn; o[7] = c->orphan;
                } else { o[1] = 0; o[3] = 0; o[4] = 0; o[5] = s_head[k] - s_tail[k]; o[6] = 0; o[7] = 0; }
                o = o + 8;
                ++n;
            }
            ++k;
        }
        net_unlock();
        return n;
    }
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
        v[18] = n_tseg; v[19] = n_trex; v[20] = n_trst;
        k = 0;
        while (k < b && k < 21) { o[k] = v[k]; ++k; }
        return k;
    }
    if (op == 1) {
        if (b < 8 || b > 30) return 0 - 1;
        klock_take(net_lk);
        my_ip = a & ALL;
        my_mask = (ALL << (32 - b)) & ALL;
        k = 0;
        while (k < NARP) { arp_ok[k] = 0; arp_plen[k] = 0; arp_ip[k] = 0; ++k; }
        net_unlock();
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
        net_unlock();
        return n == 0 - 1 ? 0 : n & ALL;
    }
    return 0 - 1;
}

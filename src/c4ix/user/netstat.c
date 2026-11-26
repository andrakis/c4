// C4IX userland: netstat -- the machine's sockets (docs/c4pc-network.md):
// each one's protocol, its local and peer's address, bytes waiting each
// way, and a TCP connection's state; then TCP's counts.

#include "c4ix_user.h"
#include "c4ix_net.h"

enum { MAXS = 32 };

static char *tstate[11];

static void states() {
    tstate[0] = "CLOSED"; tstate[1] = "LISTEN"; tstate[2] = "SYN_SENT"; tstate[3] = "SYN_RECV";
    tstate[4] = "ESTABLISHED"; tstate[5] = "FIN_WAIT1"; tstate[6] = "FIN_WAIT2"; tstate[7] = "CLOSE_WAIT";
    tstate[8] = "CLOSING"; tstate[9] = "LAST_ACK"; tstate[10] = "TIME_WAIT";
}

// "a.b.c.d:port" (or "*:port", "*:*") into OUT
static char *ep(int ip, int port, char *out) {
    char p[12];
    if (ip) uipstr(ip, out); else ustrcpy(out, "*");
    ustrcat(out, ":");
    if (port) uitoa(port, p); else ustrcpy(p, "*");
    ustrcat(out, p);
    return out;
}

int main(int argc, char **argv) {
    int v[MAXS * NS_WORDS], info[NI_WORDS], n, k, me;
    int *w;
    char l[32], r[32];
    char *proto, *st;
    states();
    if (unetinfo(info) < NI_WORDS) { ufprintf(STDERR, "netstat: this kernel has no network (or not TCP's)\n"); return 1; }
    me = info[NI_UP] ? info[NI_IP] : 0;
    n = unetsocks(v, MAXS * NS_WORDS);
    uprintf("Proto Recv-Q Send-Q Local Address          Foreign Address        State\n");
    k = 0;
    while (k < n) {
        w = v + k * NS_WORDS;
        proto = w[NS_KIND] == SOCK_TCP ? "tcp" : w[NS_KIND] == SOCK_UDP ? "udp" : "icmp";
        st = w[NS_KIND] == SOCK_TCP && w[NS_STATE] >= 0 && w[NS_STATE] <= 10 ? tstate[w[NS_STATE]] : "";
        uprintf("%-5s %6d %6d %-22s %-22s %s%s\n", proto, w[NS_IN], w[NS_OUT],
            ep(w[NS_KIND] == SOCK_TCP && w[NS_STATE] != TS_LISTEN && w[NS_STATE] != TS_CLOSED ? me : 0, w[NS_PORT], l),
            ep(w[NS_RIP] & NET_ALL, w[NS_RPORT], r), st, w[NS_ORPHAN] ? " (closed)" : "");
        ++k;
    }
    uprintf("tcp: %d segments sent, %d timeouts, %d resets sent\n", info[NI_TSEG], info[NI_TREX], info[NI_TRST]);
    return 0;
}

// C4IX userland: ping -- ICMP echo requests to a machine, and its replies
// timed (docs/c4pc-network.md).
//
//   ping [-c COUNT] [-i MS] [-s BYTES] [-W MS] HOST
//
// HOST: an address (10.4.0.2), localhost, or a machine of the hypervisor's
// by its name or as vmN (the switch knows them; VM 0 is control). COUNT
// requests (4; 0 until Ctrl-C), one every MS (1000), each with BYTES of
// data (56), each waited for MS (-W, 1000). The output is Linux's. Times
// are the machine's milliseconds: under one, "time<1 ms". Exits 0 if any
// reply came, 1 if none, 2 on a usage error.

#include "c4ix_user.h"
#include "c4ix_net.h"

static char out[1500];
static char in[1500];

static void put16(char *p, int v) { p[0] = v >> 8; p[1] = v; }
static int get16(char *p) { return (p[0] & 255) << 8 | (p[1] & 255); }

static void usage() {
    ufprintf(STDERR, "usage: ping [-c COUNT] [-i MS] [-s BYTES] [-W MS] HOST\n");
    uexit(2);
}

int main(int argc, char **argv) {
    char a[20];
    char *host;
    int count, interval, size, wait, k, fd, ip, ident, seq, sent, got, t0, t, n, from, ttl;
    int tmin, tmax, tsum, start, deadline;

    count = 4; interval = 1000; size = 56; wait = 1000; host = 0;
    k = 1;
    while (k < argc) {
        if (!ustrcmp(argv[k], "-c") && k + 1 < argc) { count = uatoi(argv[k + 1]); k = k + 2; }
        else if (!ustrcmp(argv[k], "-i") && k + 1 < argc) { interval = uatoi(argv[k + 1]); k = k + 2; }
        else if (!ustrcmp(argv[k], "-s") && k + 1 < argc) { size = uatoi(argv[k + 1]); k = k + 2; }
        else if (!ustrcmp(argv[k], "-W") && k + 1 < argc) { wait = uatoi(argv[k + 1]); k = k + 2; }
        else if (argv[k][0] == '-' || host) usage();
        else { host = argv[k]; ++k; }
    }
    if (!host || count < 0 || interval < 1 || size < 4 || size > 1472 || wait < 1) usage();
    if (!(ip = uresolve(host))) { ufprintf(STDERR, "ping: %s: unknown host\n", host); return 2; }
    if ((fd = usocket(SOCK_ICMP)) < 0) { ufprintf(STDERR, "ping: no socket (no network in this kernel?)\n"); return 2; }
    ident = ubind(fd, 0);
    uprintf("PING %s (%s) %d(%d) bytes of data.\n", host, uipstr(ip, a), size, size + 28);

    seq = 0; sent = 0; got = 0; tmin = 0; tmax = 0; tsum = 0;
    start = __time();
    while (!count || seq < count) {
        ++seq;
        out[0] = 8; out[1] = 0; put16(out + 2, 0); put16(out + 4, ident); put16(out + 6, seq);
        t0 = __time();
        out[8] = t0 >> 24; out[9] = t0 >> 16; out[10] = t0 >> 8; out[11] = t0;
        k = 12;
        while (k < 8 + size) { out[k] = k; ++k; }
        if (usendto(fd, out, 8 + size, ip, 0) < 0) {
            ufprintf(STDERR, "ping: sendto %s: no route (not on this machine's network)\n", uipstr(ip, a));
            return 1;
        }
        ++sent;
        deadline = t0 + wait;
        while (1) {
            n = urecvfrom(fd, in, sizeof in, &from, &ttl, deadline);
            if (n == NET_TIMEDOUT || n < 0) break;
            if (n < 8 || in[0] != 0 || get16(in + 4) != ident || get16(in + 6) != seq) continue;
            t = __time() - t0;
            if (!got || t < tmin) tmin = t;
            if (t > tmax) tmax = t;
            tsum = tsum + t;
            ++got;
            if (t < 1) uprintf("%d bytes from %s: icmp_seq=%d ttl=%d time<1 ms\n", n, uipstr(from, a), seq, ttl);
            else uprintf("%d bytes from %s: icmp_seq=%d ttl=%d time=%d ms\n", n, uipstr(from, a), seq, ttl, t);
            break;
        }
        if (count && seq >= count) break;
        t = t0 + interval - __time();
        if (t > 0) umsleep(t);
    }
    uprintf("\n--- %s ping statistics ---\n", host);
    uprintf("%d packets transmitted, %d received, %d%% packet loss, time %dms\n",
        sent, got, sent ? (sent - got) * 100 / sent : 0, __time() - start);
    if (got) uprintf("rtt min/avg/max = %d/%d/%d ms\n", tmin, tsum / got, tmax);
    return got ? 0 : 1;
}

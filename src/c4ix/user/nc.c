// C4IX userland: nc -- netcat's UDP mode (docs/c4pc-network.md): two
// machines talking.
//
//   nc -u [-w SECS] HOST PORT   stdin's lines to HOST:PORT; what comes back to stdout
//   nc -u -l PORT               what comes to PORT to stdout; stdin's lines
//                               back to whoever sent last
//
// A datagram a read of stdin (a line, as a terminal gives them), 1472
// bytes at most. Without -l, nc ends SECS (1) after stdin's end, having
// printed what came meanwhile: `echo hello | nc -u one 7000` is a message.
// With -l it runs until Ctrl-C. Two machines, one `nc -u -l 7000` and the
// other `nc -u NAME 7000`, are a chat. TCP's nc is the next milestone's.

#include "c4ix_user.h"
#include "c4ix_net.h"

static char buf[1500];

static void usage() {
    ufprintf(STDERR, "usage: nc -u [-w SECS] HOST PORT\n       nc -u -l PORT\n");
    uexit(2);
}

int main(int argc, char **argv) {
    char a[20];
    int udp, listen, wait, k, fd, peer, pport, n, from, fport, eof_at, avail, port;
    char *host, *ps;
    udp = 0; listen = 0; wait = 1; host = 0; ps = 0;
    k = 1;
    while (k < argc) {
        if (!ustrcmp(argv[k], "-u")) udp = 1;
        else if (!ustrcmp(argv[k], "-l")) listen = 1;
        else if (!ustrcmp(argv[k], "-ul") || !ustrcmp(argv[k], "-lu")) { udp = 1; listen = 1; }
        else if (!ustrcmp(argv[k], "-w") && k + 1 < argc) { wait = uatoi(argv[k + 1]); ++k; }
        else if (argv[k][0] == '-') usage();
        else if (!listen && !host) host = argv[k];
        else if (!ps) ps = argv[k];
        else usage();
        ++k;
    }
    if (!udp) { ufprintf(STDERR, "nc: only UDP (-u) for now: C4IX has no TCP yet\n"); return 2; }
    if (!ps || (!listen && !host)) usage();
    port = uatoi(ps);
    if (port <= 0 || port > 65535) usage();
    peer = 0; pport = 0;
    if (!listen && !(peer = uresolve(host))) { ufprintf(STDERR, "nc: %s: unknown host\n", host); return 1; }
    pport = port;
    if ((fd = usocket(SOCK_UDP)) < 0) { ufprintf(STDERR, "nc: no socket (no network in this kernel?)\n"); return 1; }
    if (ubind(fd, listen ? port : 0) < 0) { ufprintf(STDERR, "nc: port %d is taken\n", port); return 1; }
    if (listen) ufprintf(STDERR, "nc: listening on UDP port %d\n", port);
    eof_at = 0;
    while (1) {
        // what came, written out; with -l, its sender is who stdin's lines go to
        n = urecvfrom(fd, buf, sizeof buf, &from, &fport, __time() + 20);
        if (n > 0) {
            write(STDOUT, buf, n > (int)sizeof buf ? (int)sizeof buf : n);
            if (listen) { peer = from; pport = fport; }
        }
        // stdin: a read's worth out as a datagram, to the peer once there is one
        if (!eof_at) {
            avail = uavail(STDIN);
            if (avail < 0) {
                eof_at = __time();
                if (!listen && wait <= 0) return 0;
            } else if (avail > 0) {
                n = uread(STDIN, buf, avail > 1472 ? 1472 : avail);
                if (n < 0) eof_at = __time();
                else if (n > 0 && peer && usendto(fd, buf, n, peer, pport) < 0)
                    ufprintf(STDERR, "nc: sendto %s: no route\n", uipstr(peer, a));
            }
        } else if (!listen && __time() - eof_at >= wait * 1000) return 0;
    }
    return 0;
}

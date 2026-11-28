// C4IX userland: nc -- netcat (docs/c4pc-network.md): two machines talking,
// over TCP or UDP.
//
//   nc [-w SECS] HOST PORT        a TCP connection: stdin to it, it to stdout
//                                 (-w: closed after SECS with nothing either way)
//   nc -l [-w SECS] [-e PROG] PORT  one connection taken on PORT, the same;
//                                 with -e, PROG run with it as its stdin,
//                                 stdout and stderr (nc -l -e sh 2323: a shell)
//   nc -u [-w SECS] HOST PORT     UDP: stdin's lines to HOST:PORT as datagrams,
//                                 what comes back to stdout
//   nc -u -l PORT                 what comes to PORT to stdout; stdin's lines
//                                 back to whoever sent last
//
// TCP: at stdin's end nc says so (the FIN) and goes on printing until the
// other side closes too: `echo hello | nc one 7000` is a message, and
// `nc -l 7000 > file` on one machine and `cat file | nc one 7000` on
// another a copy. The other side's FIN ends nc only when stdin has ended
// or is the console (tcp_pump). A datagram is a read of stdin (a line, as a terminal
// gives them), 1472 bytes at most; UDP's nc ends SECS (1) after stdin's
// end, having printed what came meanwhile; with -l it runs until Ctrl-C.

#include "c4ix_user.h"
#include "c4ix_net.h"

static char buf[1500];

static void usage() {
    ufprintf(STDERR, "usage: nc [-u] [-w SECS] HOST PORT\n       nc -l [-u] [-e PROG] PORT\n");
    uexit(2);
}

// stdin the console (S_IFCHR): someone typing, who is done when the other
// side is
static int stdin_typed() {
    int st[ST_WORDS];
    if (ufstat(STDIN, st, ST_WORDS) < 0) return 1;
    return (st[ST_MODE] & 0xF000) == 0x2000;
}

// a connection's bytes both ways until it ends: stdin to FD (then a FIN at
// stdin's end), FD to stdout; or until IDLE ms (0: no limit) with nothing
// either way. The other side's FIN ends only its half: stdin a pipe or a
// file still goes out, to its end. `nc -l 7104 &` (its stdin ended, so its
// FIN at once) and `echo hi | nc localhost 7104`: the FIN can come before
// echo's line, which is sent all the same. Typed at the console, nc ends
// with the other side's FIN, as nothing more is wanted (`nc b 80`, a HEAD
// typed, the answer).
static int tcp_pump(int fd, int idle) {
    int p[6];
    int n, a, moved, eof, done, typed, np, last;
    eof = 0;
    done = 0;                                      // the other side has said all it will
    typed = stdin_typed();
    last = __time();
    while (1) {
        if (idle > 0 && __time() - last >= idle) return 0;
        moved = 0;
        if (!done) {
            n = uavail(fd);
            if (n < 0) {                           // the other side's FIN, or the connection's end
                if (eof || typed) return 0;
                done = 1;
            } else if (n > 0) {
                n = uread(fd, buf, n > (int)sizeof buf ? (int)sizeof buf : n);
                if (n <= 0) return 0;
                write(STDOUT, buf, n);
                moved = 1;
            }
        }
        if (!eof) {
            a = uavail(STDIN);
            if (a < 0) { eof = 1; ushutdown(fd); }
            else if (a > 0) {
                a = uread(STDIN, buf, a > (int)sizeof buf ? (int)sizeof buf : a);
                if (a <= 0) { eof = 1; ushutdown(fd); }
                else if (write(fd, buf, a) != a) { ufprintf(STDERR, "nc: the connection is gone\n"); return 1; }
                moved = 1;
            }
            if (eof && done) return 0;
        }
        // nothing either way: until the connection or stdin has something (or -w's time is up)
        if (moved) last = __time();
        else {
            np = 0;
            if (!done) { p[0] = fd; p[1] = POLL_IN; np = 1; }
            if (!eof) { p[np * 3] = STDIN; p[np * 3 + 1] = POLL_IN; ++np; }
            a = 0 - 1;
            if (idle > 0 && (a = idle - (__time() - last)) < 0) a = 0;
            upoll(p, np, a);
        }
    }
    return 0;
}

// PROG run with the connection FD as its stdin, stdout and stderr; its exit code
// (as sh starts a program: nc's own descriptors pointed at it, spawned)
static int tcp_exec(int fd, char *prog) {
    char *argv[2];
    char path[80];
    int pid;
    if (prog[0] == '/') ustrcpy(path, prog);
    else { ustrcpy(path, "/bin/"); ustrcat(path, prog); }
    argv[0] = prog;
    argv[1] = 0;
    udup2(fd, STDIN);
    udup2(fd, STDOUT);
    udup2(fd, STDERR);
    close(fd);
    pid = spawn(path, 1, argv);
    close(STDIN);
    close(STDOUT);
    if (pid < 0) return 127;
    return uwait(pid);
}

static int tcp_main(int listen, char *host, int port, char *prog, int idle) {
    char a[20];
    int fd, c, r, ip, pp;
    if ((fd = usocket(SOCK_TCP)) < 0) { ufprintf(STDERR, "nc: no socket (no network in this kernel?)\n"); return 1; }
    if (!listen) {
        if (!(ip = uresolve(host))) { ufprintf(STDERR, "nc: %s: unknown host\n", host); return 1; }
        r = uconnect(fd, ip, port);
        if (r == NET_REFUSED) { ufprintf(STDERR, "nc: %s port %d: connection refused\n", host, port); return 1; }
        if (r == NET_TIMEDOUT) { ufprintf(STDERR, "nc: %s port %d: timed out\n", host, port); return 1; }
        if (r < 0) { ufprintf(STDERR, "nc: %s port %d: no route\n", host, port); return 1; }
        return tcp_pump(fd, idle);
    }
    if (ubind(fd, port) < 0) { ufprintf(STDERR, "nc: port %d is taken\n", port); return 1; }
    if (ulisten(fd, 1) < 0) { ufprintf(STDERR, "nc: cannot listen on %d\n", port); return 1; }
    ufprintf(STDERR, "nc: listening on TCP port %d\n", port);
    if ((c = uaccept(fd, &ip, &pp)) < 0) { ufprintf(STDERR, "nc: accept failed\n"); return 1; }
    close(fd);
    ufprintf(STDERR, "nc: connection from %s port %d\n", uipstr(ip, a), pp);
    if (prog) return tcp_exec(c, prog);
    return tcp_pump(c, idle);
}

int main(int argc, char **argv) {
    char a[20];
    int udp, listen, wait, wset, k, fd, peer, pport, n, from, fport, eof_at, avail, port;
    char *host, *ps, *prog;
    udp = 0; listen = 0; wait = 1; wset = 0; host = 0; ps = 0; prog = 0;
    k = 1;
    while (k < argc) {
        if (!ustrcmp(argv[k], "-u")) udp = 1;
        else if (!ustrcmp(argv[k], "-l")) listen = 1;
        else if (!ustrcmp(argv[k], "-ul") || !ustrcmp(argv[k], "-lu")) { udp = 1; listen = 1; }
        else if (!ustrcmp(argv[k], "-w") && k + 1 < argc) { wait = uatoi(argv[k + 1]); wset = 1; ++k; }
        else if (!ustrcmp(argv[k], "-e") && k + 1 < argc) { prog = argv[k + 1]; ++k; }
        else if (argv[k][0] == '-') usage();
        else if (!listen && !host) host = argv[k];
        else if (!ps) ps = argv[k];
        else usage();
        ++k;
    }
    if (!ps || (!listen && !host) || (prog && (udp || !listen))) usage();
    port = uatoi(ps);
    if (port <= 0 || port > 65535) usage();
    if (!udp) return tcp_main(listen, host, port, prog, wset ? wait * 1000 : 0);
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

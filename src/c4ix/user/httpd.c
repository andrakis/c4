// C4IX userland: httpd -- a small web server (docs/c4pc-network.md): a
// directory's files over HTTP/1.0, GET and HEAD.
//
//   httpd [-p PORT] [-q] [-d] [-n COUNT] [DIR]
//                                 DIR (/ if not given) on PORT (80);
//                                 -q: no line per request on stdout;
//                                 -d: in the background (a copy of it
//                                 started, and this one done);
//                                 -n: COUNT answers, then done
//
// One task, its connections at once: upoll waits for whichever has
// something -- a connection to take, a request's bytes, room to send --
// and each connection's socket does not wait (uaccept4's C4IX_O_NONBLOCK).
// A directory is its index.html if it has one, else a list of it. A
// connection is closed once its answer is sent, or after 10 s with
// nothing (HTTP/1.0's Connection: close); NC at once, a 503 for more.
// A request ends at its empty line, or at the client's end (FIN):
// `echo "GET / HTTP/1.0" | nc host 80` is a request.

#include "c4ix_user.h"
#include "c4ix_net.h"

enum {
    NC = 16, NPOLL = 17, RQ = 1024, OB = 2048, PATHMAX = 256, IDLE = 10000,
    C_FREE = 0, C_READ = 1, C_SEND = 2,
    O_NONBLOCK = 2048
};

static char *root;
static int quiet;
static int lfd;
static int c_fd[NC], c_st[NC], c_rn[NC], c_ff[NC], c_dir[NC], c_di[NC], c_on[NC], c_oo[NC], c_last[NC], c_ip[NC];
static int c_eof[NC];              // the answer's last bytes are in the buffer
static char c_rq[NC * RQ];         // the request as it comes
static char c_ob[NC * OB];         // what is to be sent
static char c_path[NC * PATHMAX];  // a directory being listed
static int served;

static char *cat(char *p, char *s) { while (*s) *p++ = *s++; *p = 0; return p; }
static char *catn(char *p, int v) { char b[12]; return cat(p, uitoa(v, b)); }

static int ends(char *s, char *suf) {
    int a, b;
    a = ustrlen(s); b = ustrlen(suf);
    return a >= b && !ustrcmp(s + a - b, suf);
}

static char *ctype(char *path) {
    if (ends(path, ".html") || ends(path, ".htm")) return "text/html";
    if (ends(path, ".txt") || ends(path, ".c") || ends(path, ".h") || ends(path, ".md") || ends(path, ".sh")) return "text/plain";
    if (ends(path, ".css")) return "text/css";
    if (ends(path, ".js") || ends(path, ".mjs")) return "text/javascript";
    if (ends(path, ".json")) return "application/json";
    if (ends(path, ".png")) return "image/png";
    if (ends(path, ".jpg") || ends(path, ".jpeg")) return "image/jpeg";
    if (ends(path, ".gif")) return "image/gif";
    if (ends(path, ".svg")) return "image/svg+xml";
    return "application/octet-stream";
}

static int hexv(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return 0 - 1;
}

// the request's path (to a space, ?, or its end) decoded into OUT, MAX at
// most; 0 if it is not one we serve (not from /, a .. in it, too long)
static int urlpath(char *s, char *out, int max) {
    char *o;
    int a, b, n;
    o = out; n = 0;
    if (*s != '/') return 0;
    while (*s && *s != ' ' && *s != '?' && *s != '\r' && *s != '\n') {
        if (n + 1 >= max) return 0;
        if (*s == '%' && (a = hexv(s[1])) >= 0 && (b = hexv(s[2])) >= 0) { *o++ = a * 16 + b; s = s + 3; }
        else *o++ = *s++;
        ++n;
    }
    *o = 0;
    o = out;
    while (*o) {
        if (o[0] == '.' && o[1] == '.' && (o == out || o[-1] == '/') && (o[2] == '/' || !o[2])) return 0;
        ++o;
    }
    return 1;
}

static void closeconn(int k) {
    if (c_ff[k] >= 0) uclose(c_ff[k]);
    uclose(c_fd[k]);
    c_st[k] = C_FREE;
}

// the head of an answer into K's buffer: CODE, its words, a type and a length (-1: none)
static char *head(int k, int code, char *words, char *type, int len) {
    char *p;
    p = cat(c_ob + k * OB, "HTTP/1.0 ");
    p = catn(p, code);
    p = cat(p, " ");
    p = cat(p, words);
    p = cat(p, "\r\nServer: c4ix-httpd\r\nConnection: close\r\nContent-Type: ");
    p = cat(p, type);
    if (len >= 0) { p = cat(p, "\r\nContent-Length: "); p = catn(p, len); }
    p = cat(p, "\r\n\r\n");
    return p;
}

// an answer with a short page of its own
static void fail(int k, int code, char *words, int body) {
    char *p, *s;
    char page[160];
    s = cat(page, "<html><body><h1>");
    s = catn(s, code);
    s = cat(s, " ");
    s = cat(s, words);
    s = cat(s, "</h1></body></html>\n");
    p = head(k, code, words, "text/html", ustrlen(page));
    if (body) p = cat(p, page);
    c_on[k] = p - (c_ob + k * OB);
    c_oo[k] = 0;
    c_eof[k] = 1;
}

static void logline(int k, char *m, char *path, int code) {
    char a[20];
    if (quiet) return;
    uprintf("%s %s %s %d\n", uipstr(c_ip[k], a), m, path, code);
}

// a request in K's buffer: its answer begun
static void answer(int k) {
    char path[PATHMAX], file[PATHMAX * 2], *r, *p, *m;
    int st[4], body, n;
    r = c_rq + k * RQ;
    c_st[k] = C_SEND;
    c_ff[k] = 0 - 1;
    c_dir[k] = 0;
    body = 1;
    ++served;
    if (ustarts(r, "GET ")) { m = "GET"; r = r + 4; }
    else if (ustarts(r, "HEAD ")) { m = "HEAD"; r = r + 5; body = 0; }
    else { fail(k, 501, "Not Implemented", 1); logline(k, "?", "-", 501); return; }
    if (!urlpath(r, path, PATHMAX)) { fail(k, 400, "Bad Request", body); logline(k, m, "-", 400); return; }
    n = upathjoin(file, sizeof file, root, path + 1);
    if (n < 0 || ustat(file, st) < 0) { fail(k, 404, "Not Found", body); logline(k, m, path, 404); return; }
    if (st[0] == 1 && !ends(path, "/")) {
        // a directory named without its /: there, so that its links are its own
        p = cat(c_ob + k * OB, "HTTP/1.0 301 Moved Permanently
Server: c4ix-httpd
Connection: close
Location: ");
        p = cat(p, path);
        p = cat(p, "/
Content-Length: 0

");
        c_on[k] = p - (c_ob + k * OB);
        c_oo[k] = 0;
        c_eof[k] = 1;
        logline(k, m, path, 301);
        return;
    }
    if (st[0] == 1) {
        // a directory: its index.html, or a list of it
        p = file + ustrlen(file);
        if (p > file && p[-1] != '/') { *p++ = '/'; *p = 0; }
        ustrcat(file, "index.html");
        if (ustat(file, st) < 0 || st[0] != 2) {
            *p = 0;
            ustrcpy(c_path + k * PATHMAX, file);
            p = head(k, 200, "OK", "text/html", 0 - 1);
            if (body) {
                p = cat(p, "<html><head><title>");
                p = cat(p, path);
                p = cat(p, "</title></head><body><h1>");
                p = cat(p, path);
                p = cat(p, "</h1><ul>\n");
                c_dir[k] = 1;
                c_di[k] = 0;
            }
            c_on[k] = p - (c_ob + k * OB);
            c_oo[k] = 0;
            c_eof[k] = !body;
            logline(k, m, path, 200);
            return;
        }
    }
    if (st[0] != 2 || (c_ff[k] = uopen(file, 0)) < 0) { c_ff[k] = 0 - 1; fail(k, 403, "Forbidden", body); logline(k, m, path, 403); return; }
    p = head(k, 200, "OK", ctype(file), st[1]);
    c_on[k] = p - (c_ob + k * OB);
    c_oo[k] = 0;
    c_eof[k] = !body;
    logline(k, m, path, 200);
}

// K's buffer sent: more of its answer into it, or the end
static void refill(int k) {
    char name[64], *b, *p, *dir;
    int n, d, room;
    b = c_ob + k * OB;
    c_on[k] = 0;
    c_oo[k] = 0;
    if (c_eof[k]) { closeconn(k); return; }
    if (c_ff[k] >= 0) {
        n = uread(c_ff[k], b, OB);
        if (n <= 0) { closeconn(k); return; }
        c_on[k] = n;
        return;
    }
    if (c_dir[k]) {
        // a list's next lines, each an entry; then its end
        dir = c_path + k * PATHMAX;
        p = b;
        room = 1;
        while (room) {
            d = ureaddir(dir, c_di[k], name);
            if (d < 0) {
                p = cat(p, "</ul></body></html>\n");
                c_eof[k] = 1;
                room = 0;
            } else {
                ++c_di[k];
                p = cat(p, "<li><a href=\"");
                p = cat(p, name);
                if (d == 1) p = cat(p, "/");
                p = cat(p, "\">");
                p = cat(p, name);
                if (d == 1) p = cat(p, "/");
                p = cat(p, "</a></li>\n");
                if (p - b > OB - 200) room = 0;
            }
        }
        c_on[k] = p - b;
        return;
    }
    closeconn(k);
}

// a new connection: a slot for it, or a 503
static void take() {
    int k, fd, ip, port;
    char *p;
    if ((fd = uaccept4(lfd, &ip, &port, O_NONBLOCK)) < 0) return;
    k = 0;
    while (k < NC && c_st[k] != C_FREE) ++k;
    if (k == NC) {
        p = "HTTP/1.0 503 Service Unavailable\r\nConnection: close\r\nContent-Length: 0\r\n\r\n";
        write(fd, p, ustrlen(p));
        uclose(fd);
        return;
    }
    c_fd[k] = fd;
    c_st[k] = C_READ;
    c_rn[k] = 0;
    c_ff[k] = 0 - 1;
    c_ip[k] = ip;
    c_last[k] = __time();
}

// K's socket has bytes, or its end: the request's
static void readreq(int k) {
    char *r;
    int n, i, done;
    r = c_rq + k * RQ;
    n = uread(c_fd[k], r + c_rn[k], RQ - 1 - c_rn[k]);
    if (n < 0) return;                         // nothing after all
    done = n == 0;
    if (n == 0 && c_rn[k] == 0) { closeconn(k); return; }
    c_rn[k] = c_rn[k] + n;
    r[c_rn[k]] = 0;
    // the empty line that ends the head: \r\n\r\n, or \n\n
    i = 0;
    while (!done && i + 1 < c_rn[k]) {
        if (r[i] == '\n' && (r[i + 1] == '\n' || (r[i + 1] == '\r' && r[i + 2] == '\n'))) done = 1;
        ++i;
    }
    if (!done && c_rn[k] >= RQ - 1) { fail(k, 413, "Request Too Large", 1); c_st[k] = C_SEND; return; }
    if (done) answer(k);
}

// K's socket has room: what its buffer holds, as much as goes
static void sendsome(int k) {
    int n;
    if (c_oo[k] >= c_on[k]) refill(k);
    if (c_st[k] == C_FREE || c_on[k] == 0) return;
    n = write(c_fd[k], c_ob + k * OB + c_oo[k], c_on[k] - c_oo[k]);
    if (n < 0) { closeconn(k); return; }
    c_oo[k] = c_oo[k] + n;
    if (c_oo[k] >= c_on[k] && c_eof[k]) closeconn(k);
}

static void usage() {
    ufprintf(STDERR, "usage: httpd [-p PORT] [-q] [-d] [-n COUNT] [DIR]\n");
    uexit(2);
}

int main(int argc, char **argv) {
    int p[NPOLL * 3], who[NPOLL];
    char *cargv[12];
    int port, k, n, i, st[4], now, ev, bg, limit, busy;
    port = 80; root = "/"; quiet = 0; bg = 0; limit = 0;
    // the arguments, and the same but -d for the copy -d starts
    cargv[0] = "httpd";
    n = 1;
    k = 1;
    while (k < argc) {
        if (!ustrcmp(argv[k], "-d")) bg = 1;
        else {
            if (n < 11) { cargv[n] = argv[k]; ++n; }
            if ((!ustrcmp(argv[k], "-p") || !ustrcmp(argv[k], "-n")) && k + 1 < argc) {
                if (argv[k][1] == 'p') port = uatoi(argv[k + 1]); else limit = uatoi(argv[k + 1]);
                ++k;
                if (n < 11) { cargv[n] = argv[k]; ++n; }
            }
            else if (!ustrcmp(argv[k], "-q")) quiet = 1;
            else if (argv[k][0] == '-') usage();
            else root = argv[k];
        }
        ++k;
    }
    if (port <= 0 || port > 65535) usage();
    if (ustat(root, st) < 0 || st[0] != 1) { ufprintf(STDERR, "httpd: %s is not a directory\n", root); return 1; }
    if (bg) {
        cargv[n] = 0;
        if ((k = spawn("/bin/httpd", n, cargv)) < 0) { ufprintf(STDERR, "httpd: cannot start /bin/httpd\n"); return 1; }
        ufprintf(STDERR, "httpd: in the background, task %d\n", k);
        return 0;
    }
    if ((lfd = usocket(SOCK_TCP)) < 0) { ufprintf(STDERR, "httpd: no socket (no network in this kernel?)\n"); return 1; }
    if (ubind(lfd, port) < 0) { ufprintf(STDERR, "httpd: port %d is taken\n", port); return 1; }
    if (ulisten(lfd, 8) < 0) { ufprintf(STDERR, "httpd: cannot listen on %d\n", port); return 1; }
    ufprintf(STDERR, "httpd: serving %s on TCP port %d\n", root, port);
    k = 0;
    while (k < NC) { c_st[k] = C_FREE; ++k; }
    while (1) {
        // the listener, and each connection: its request's bytes, or room to send
        p[0] = lfd; p[1] = POLL_IN; p[2] = 0;
        n = 1;
        now = __time();
        k = 0;
        while (k < NC) {
            if (c_st[k] != C_FREE && now - c_last[k] >= IDLE) closeconn(k);
            if (c_st[k] != C_FREE) {
                p[n * 3] = c_fd[k];
                p[n * 3 + 1] = c_st[k] == C_READ ? POLL_IN : POLL_OUT;
                p[n * 3 + 2] = 0;
                who[n] = k;
                ++n;
            }
            ++k;
        }
        if (upoll(p, n, n > 1 ? 1000 : 0 - 1) < 0) { ufprintf(STDERR, "httpd: poll failed\n"); return 1; }
        now = __time();
        i = 1;
        while (i < n) {
            k = who[i];
            ev = p[i * 3 + 2];
            if (ev) c_last[k] = now;
            if (ev & (POLL_ERR | POLL_NVAL)) closeconn(k);
            else if (c_st[k] == C_READ && (ev & (POLL_IN | POLL_HUP))) readreq(k);
            else if (c_st[k] == C_SEND && (ev & POLL_OUT)) sendsome(k);
            else if (c_st[k] == C_SEND && (ev & POLL_HUP)) closeconn(k);
            ++i;
        }
        // -n: its answers given, and sent, it is done
        busy = 0; k = 0; while (k < NC) { if (c_st[k] != C_FREE) busy = 1; ++k; }
        if (limit > 0 && served >= limit && !busy) { uclose(lfd); return 0; }
        if (p[2] & POLL_IN) take();
    }
    return 0;
}

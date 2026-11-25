// C4IX userland: dmesg -- print the kernel's log.
//
// The log is the kernel's ring of every line it has said since it
// booted, each stamped with the machine's time (console.c), and
// /proc/kmsg is that ring as a file, made when it is read. Unlike
// Linux's /proc/kmsg, reading it takes nothing out.
//
//   dmesg        the whole log
//   dmesg -t     without the times
//   dmesg -w     the whole log, then each new line as the kernel says it,
//                until Ctrl-C (Linux's --follow; -tw, -wt or -t -w both)
//
// Following reads the ring again four times a second and prints what
// comes after the last line it printed, found by its text: the stamp
// makes each line all but unique. Lines the ring lost before they were
// read -- more than its 64 KB said between two looks -- are not printed,
// and a line says so.

#include "c4ix_user.h"

enum { KMSG_MAX = 65536 + 4096, LAST_MAX = 256, FOLLOW_MS = 250 };

static int notime;
static int mid, skip;            // -t's state across writes
static char last[LAST_MAX];      // the last whole line printed, with its stamp
static int nlast;

static void put(char *buf, int n) {
    int i, k;
    if (!notime) { write(STDOUT, buf, n); return; }
    // -t: each line from after its "] "
    k = 0;
    i = 0;
    while (i < n) {
        if (!mid) { mid = 1; skip = buf[i] == '['; }
        if (skip) {
            if (buf[i] == ']') { skip = 2; }
            else if (skip == 2 && buf[i] == ' ') { skip = 0; }
            ++i;
            continue;
        }
        buf[k] = buf[i];
        ++k;
        if (buf[i] == '\n') mid = 0;
        ++i;
    }
    if (k) write(STDOUT, buf, k);
}

// The whole of /proc/kmsg into BUF: its length, or -1
static int snap(char *buf) {
    int fd, n, got;
    if ((fd = uopen("/proc/kmsg", O_RD)) < 0) return 0 - 1;
    got = 0;
    while (got < KMSG_MAX && (n = uread(fd, buf + got, KMSG_MAX - got)) > 0) got = got + n;
    uclose(fd);
    return got;
}

// Where the line that was printed last ends in BUF's N bytes (the offset
// after its newline), looking from the end; -1 if it is not there
static int after_last(char *buf, int n) {
    int e, s, k;
    if (!nlast) return 0;
    e = n;
    while (e > 0) {
        // buf[s..e) a whole line, its newline at e - 1
        if (buf[e - 1] == '\n') {
            s = e - 1;
            while (s > 0 && buf[s - 1] != '\n') --s;
            if (e - s == nlast) {
                k = 0;
                while (k < nlast && buf[s + k] == last[k]) ++k;
                if (k == nlast) return e;
            }
            e = s;
        } else --e;
    }
    return 0 - 1;
}

// Keep the last whole line of BUF's N bytes as the one printed last
static void remember(char *buf, int n) {
    int e, s;
    e = n;
    while (e > 0 && buf[e - 1] != '\n') --e;
    if (!e) return;
    s = e - 1;
    while (s > 0 && buf[s - 1] != '\n') --s;
    if (e - s > LAST_MAX) { nlast = 0; return; }
    nlast = e - s;
    memcpy(last, buf + s, nlast);
}

int main(int argc, char **argv) {
    char *buf;
    int k, i, n, from, follow, end;

    notime = 0;
    follow = 0;
    k = 1;
    while (k < argc) {
        if (argv[k][0] != '-' || !argv[k][1]) break;
        i = 1;
        while (argv[k][i] == 't' || argv[k][i] == 'w') {
            if (argv[k][i] == 't') notime = 1; else follow = 1;
            ++i;
        }
        if (argv[k][i]) break;
        ++k;
    }
    if (k < argc) {
        ufprintf(STDERR, "usage: dmesg [-t] [-w]\n");
        return 2;
    }
    if (!(buf = (char *)ualloc(KMSG_MAX))) {
        ufprintf(STDERR, "dmesg: no memory\n");
        return 1;
    }
    if ((n = snap(buf)) < 0) {
        ufprintf(STDERR, "dmesg: cannot open /proc/kmsg\n");
        return 1;
    }
    // only whole lines while following: a line half said is printed once
    // it is all there
    end = n;
    if (follow) while (end > 0 && buf[end - 1] != '\n') --end;
    remember(buf, end);
    put(buf, end);
    while (follow) {
        umsleep(FOLLOW_MS);
        if ((n = snap(buf)) < 0) return 1;
        end = n;
        while (end > 0 && buf[end - 1] != '\n') --end;
        from = after_last(buf, end);
        if (from < 0) {
            write(STDOUT, "dmesg: (lines lost: the log ran on faster than it was read)\n", 61);
            from = 0;
        }
        if (end > from) {
            remember(buf, end);
            put(buf + from, end - from);
        }
    }
    return 0;
}

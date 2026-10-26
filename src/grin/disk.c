// disk.c -- grin and C4DOS's files (docs/grin-plan.md 3.8 and 4): what
// File Manager and the kit's file box ask DOS -- which drives there are,
// which one files are written to, what is on each, how big a file is --
// and the kit's host functions for files, which open through libc and
// bare.c, so through DOS.
//
// A drive is listed by its c4dos.dir, which DOS keeps up to date on any
// drive it writes (HOME=B:); the RAM disk by DOS's own slots (9-12).
// Neither says how big a file is, short of reading it, so a size is
// read when it is asked for, a file at a time.

#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include "shell.h"
#include "dospath.h"

extern int *__c4dos_api;              // bare.c's: C4DOS's table
extern int __c4ke_exit;               // bare.c's: set by C4KE's loader, when grin is a C4KE task

enum { SLOT_VERSION = 5, SLOT_OPEN = 6, SLOT_READ = 7, SLOT_RCLOSE = 8, SLOT_COUNT = 9,
       SLOT_ENTNAME = 10, SLOT_ENTSIZE = 11, SLOT_UNLINK = 18, SLOT_EXEC = 19,
       SLOT_SINK = 24, SLOT_SETGEN = 27, SLOT_HOME = 28, SLOT_DRIVES = 29, SLOT_DRIVE = 30 };
enum { DIRBUF = 16384, CHUNK = 4096 };

static int ver() { return __c4dos_api ? __c4dos_api[SLOT_VERSION] : 0; }
static int slot(int s) { return ver() >= 2 && __c4dos_api[s]; }
static int call1(int s, int a) {
    int (*f)(int);
    f = (int (*)(int))__c4dos_api[s];
    return f(a);
}
static int call3(int s, int a, int b, int c) {
    int (*f)(int, int, int);
    f = (int (*)(int, int, int))__c4dos_api[s];
    return f(a, b, c);
}

static int ieq(char *a, char *b) {
    while (*a && (*a | 32) == (*b | 32)) { ++a; ++b; }
    return !*a && !*b;
}

int gd_drives() { return ver() >= 5 && slot(SLOT_DRIVES) ? call1(SLOT_DRIVES, 0) : 1; }
int gd_home() { return ver() >= 5 && slot(SLOT_HOME) ? call1(SLOT_HOME, 0) : 0 - 1; }
int gd_unlink(char *name) { return ver() >= 4 && slot(SLOT_UNLINK) ? call1(SLOT_UNLINK, (int)name) : 0 - 1; }
int gd_exec(char *line) { return ver() >= 4 && slot(SLOT_EXEC) ? call1(SLOT_EXEC, (int)line) : 0 - 1; }
int gd_sink(int w, int r, int ctx) { return ver() >= 4 && slot(SLOT_SINK) ? call3(SLOT_SINK, w, r, ctx) : 0 - 1; }
int gd_setgen(int g) { return ver() >= 4 && slot(SLOT_SETGEN) ? call1(SLOT_SETGEN, g) : 0 - 1; }
int gd_drive() { return ver() >= 6 && slot(SLOT_DRIVE) ? call1(SLOT_DRIVE, 0) : 0; }

// "B:" for drive 1, "RAM:" for the RAM disk, and a name after it, in out
char *gd_name(int drive, char *name, char *out, int n) {
    char d[8];
    if (drive == DRV_RAM) ui_cpy(d, "RAM:");
    else { d[0] = 'A' + drive; d[1] = ':'; d[2] = 0; }
    ui_ncpy(out, d, n);
    ui_ncat(out, name, n);
    return out;
}

// a file's size, by reading it through DOS; -1 if it does not open
static char chunk[CHUNK];
int gd_size(char *name) {
    int h, n, total;
    if (!slot(SLOT_OPEN) || (h = call1(SLOT_OPEN, (int)name)) < 0) return 0 - 1;
    total = 0;
    while ((n = call3(SLOT_READ, h, (int)chunk, CHUNK)) > 0) total = total + n;
    call1(SLOT_RCLOSE, h);
    return total;
}

// Drive `drive`'s files (DRV_RAM: the RAM disk's) into names, WK_NAME
// bytes each, at most max of them; how many. c4dos.dir itself is left
// out.
static char dirbuf[DIRBUF];
int gd_list(int drive, char *names, int max) {
    int fd, n, got, i, k;
    char path[16], *p;
    k = 0;
    if (drive == DRV_RAM) {
        if (!slot(SLOT_COUNT)) return 0;
        n = call1(SLOT_COUNT, 0);
        i = 0;
        while (i < n && k < max) {
            if ((p = (char *)call1(SLOT_ENTNAME, i))) { ui_ncpy(names + k * WK_NAME, p, WK_NAME); ++k; }
            ++i;
        }
        return k;
    }
    gd_name(drive, "c4dos.dir", path, 16);
    if ((fd = open(path, O_RDONLY)) < 0) return 0;
    got = 0;
    while (got < DIRBUF - 1 && (n = read(fd, dirbuf + got, DIRBUF - 1 - got)) > 0) got = got + n;
    close(fd);
    dirbuf[got] = 0;
    p = dirbuf;
    while (*p && k < max) {
        i = 0;
        while (p[i] && p[i] != '\n' && p[i] != '\r') ++i;
        if (i > 0 && i < WK_NAME) {
            memcpy(names + k * WK_NAME, p, i);
            names[k * WK_NAME + i] = 0;
            if (!ieq(names + k * WK_NAME, "c4dos.dir")) ++k;
        }
        p = p + i;
        while (*p == '\n' || *p == '\r') ++p;
    }
    return k;
}

// ---- the kit's host functions for files --------------------------------------------
//
// Paths are the kit's (dospath.h). wk_readdir lists the root as the
// drives and RAM:, and a drive as its files.

static char dospath[WK_PATH];
int wk_fopen(char *path, int wr) {
    return open(grin_dospath(path, dospath, WK_PATH), wr ? O_WRONLY | O_CREAT | O_TRUNC : O_RDONLY);
}
int wk_fread(int fd, char *buf, int n) { return read(fd, buf, n); }
int wk_fwrite(int fd, char *buf, int n) { return write(fd, buf, n); }
void wk_fclose(int fd) { close(fd); }

// which drive a kit folder is: "/B:" or "/B:/" -> 1, "/RAM:" -> DRV_RAM;
// -1 for the root, -2 for anything else
static int folder_drive(char *dir) {
    int c;
    if (!dir[0] || (dir[0] == '/' && !dir[1])) return 0 - 1;
    if (dir[0] != '/') return 0 - 2;
    c = dir[1] | 32;
    if (c >= 'a' && c <= 'z' && dir[2] == ':' && (!dir[3] || (dir[3] == '/' && !dir[4]))) return c - 'a';
    if (c == 'r' && (dir[2] | 32) == 'a' && (dir[3] | 32) == 'm' && dir[4] == ':' && (!dir[5] || (dir[5] == '/' && !dir[6]))) return DRV_RAM;
    return 0 - 2;
}

enum { RD_MAX = 256 };
static char *rd_names;
static int rd_n;
int wk_readdir(char *dir, int i, char *name) {
    int d, n;
    d = folder_drive(dir);
    if (d == 0 - 1) {
        n = gd_drives();
        if (i < n) { name[0] = 'A' + i; name[1] = ':'; name[2] = 0; return 1; }
        if (i == n && slot(SLOT_COUNT)) { ui_cpy(name, "RAM:"); return 1; }
        return 0 - 1;
    }
    if (d < 0) return 0 - 1;
    if (i == 0) {
        if (!rd_names && !(rd_names = (char *)malloc(RD_MAX * WK_NAME))) return 0 - 1;
        rd_n = gd_list(d, rd_names, RD_MAX);
    }
    if (!rd_names || i >= rd_n) return 0 - 1;
    ui_ncpy(name, rd_names + i * WK_NAME, WK_NAME);
    return 0;
}

// ---- under C4KE --------------------------------------------------------------
//
// grin also runs as a C4KE task (the Build Ladder, docs/c4dos-ladder.md),
// with no DOS under it. A DOS Prompt there runs C4KE's programs as tasks
// of their own, and shows what they print through the machine's console
// tap (docs/c4dos-design.md "The console tap"): CON_TAP keeps what is
// printed, CON_LEN copies it out, CON_IN feeds fd 0 a line.

enum { OP_REQUEST_SYMBOL = 128, KE_PRIV_USER = 1,
       CON_TAP = 0x1d0, CON_ADDR = 0x1d4, CON_LEN = 0x1d8, CON_IN = 0x1dc };
static int op_start, op_running, op_kill;

int gd_c4ke() { return !__c4dos_api && __c4ke_exit; }

int gd_ke_start(int argc, char **argv) {
    if (!op_start) op_start = __c4_opcode("OP_USER_START_C4R", OP_REQUEST_SYMBOL);
    if (!op_start) return 0;
    return __c4_opcode(KE_PRIV_USER, argv[0], argv, argc, op_start);
}

int gd_ke_running(int pid) {
    if (!op_running) op_running = __c4_opcode("OP_KERN_TASK_RUNNING", OP_REQUEST_SYMBOL);
    if (!op_running) return 0;
    return __c4_opcode(pid, op_running);
}

// (arguments reversed: the kernel reads sp[1], the pid, first)
int gd_ke_kill(int pid, int sig) {
    if (!op_kill) op_kill = __c4_opcode("OP_USER_KILL", OP_REQUEST_SYMBOL);
    if (!op_kill) return 0 - 1;
    return __c4_opcode(sig, pid, op_kill);
}

void gd_tap(int on) { *(int *)CON_TAP = on; }
int gd_tap_drain(char *buf, int n) { *(int *)CON_ADDR = (int)buf; *(int *)CON_LEN = n; return *(int *)CON_LEN; }
void gd_tap_feed(char *buf, int n) { *(int *)CON_ADDR = (int)buf; *(int *)CON_IN = n; }
int gd_tap_left() { return *(int *)CON_IN; }

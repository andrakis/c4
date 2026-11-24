// machines.c -- Machines: the hypervisor's machines, as Proxmox shows them
// (c4pc --hv: docs/c4pc-hypervisor.md, stage 3). The control C4IX's
// desktop has it in the Go menu when the hypervisor is there.
//
// A tree on the left: the PC, and under it every machine -- each
// /vm/NAME/vm.cfg, and each machine the hypervisor has made. On the right
// the one chosen: a toolbar (Start, Shutdown, Stop, Restart, Console) and
// tabs -- Summary (its state, memory, the instructions it runs a second
// with a minute's history, its disks, and its screen small), Console (its
// screen larger; Attach gives it the PC's screen, keyboard and mouse
// until Ctrl+Alt+Home; Pause stops it where it is, Resume goes on; Delete
// takes it out and removes /vm/NAME, once asked), Hardware and Options
// (vm.cfg, a row each: a click
// edits it, for the next start), and Log (c4pc's: the PC's own, or what a
// machine's console said). Create VM asks a name, the system (C4IX's
// desktop or its text console, C4KE, C4DOS, or C4IX with an ISO file as
// its CD), the memory and a new disk image's size, and runs `vm create`
// in a Terminal, which shows the image being written. A start that fails
// says why: `vm up`'s words, kept in /vm/NAME/start.log.
//
// The machine manager's window (HV_BASE) is read directly: state, info,
// start, stop, attach and THUMB, its screen scaled into our memory. What
// is slow -- `vm up`, which reads the kernel and the boot archive, and
// `vm create` -- runs as `vm`, in the background or in a Terminal.

#include "desktop.h"

enum { R_A0 = 0x564, R_A1 = 0x568, R_A2 = 0x56c, R_A3 = 0x570, R_CMD = 0x574, R_RES = 0x578, R_COUNT = 0x57c };
enum { C_START = 5, C_STOP = 6, C_ATTACH = 7, C_STATE = 9, C_INFO = 10, C_DELETE = 11, C_THUMB = 16, C_LOG = 17,
       C_PAUSE = 18, C_RESUME = 19 };
enum { S_FREE = 0, S_MADE = 1, S_RUN = 2, S_HALT = 3, S_PAUSE = 4, S_NONE = 9 };
// the toolbar's buttons, right to left from Delete: 74 wide, 78 apart
enum { N_TB = 7, TB = 78, TBW = 74, A_DEL = 45 };
enum { MAXM = 16, NAMEW = 32, HIST = 60, TREE_W = 160, TOOL_H = 34, TAB_Y = 40, PAGE_Y = 62, ROW_H = 22,
       TW = 192, TH = 144, CW = 384, CH = 288, STRIP = 36, IMG_ID = 300,
       A_NAME = 40, A_MB = 41, A_DISK = 42, A_SYS = 43, A_EDIT = 50, N_KEYS = 10, N_TABS = 5,
       K_SYSTEM = 4, K_BOOTFROM = 5, LOGW = 32768, LOG_MAX = 600 };

struct machines {
    int n, sel, tab;                     // sel: 0 the PC, 1.. a machine
    char name[MAXM * NAMEW];
    int id[MAXM], state[MAXM], mb[MAXM], cfg[MAXM];
    long long cyc[MAXM];
    int rate[MAXM];                      // thousands of instructions a second
    int hist[MAXM * HIST], nhist[MAXM];
    int at_ms;
    int pid;                             // a `vm up` running, or 0
    char job[NAMEW];
    int restart;                         // a machine to start again once it has stopped: its id, or 0
    char status[120];
    int shown;                           // which machine's screen the images are (its id), 0 none
    char nm[NAMEW]; int nmb, ndisk;      // Create VM's answers so far
    char nsys[128];
};

static struct machines *S(struct win *w) { return (struct machines *)w->st; }
static char buf[160], info_buf[512], cfg[2048], path[128], vmpath[96];
static int thumb[CW * CH];
static char *tabs[N_TABS];
// Hardware's rows (0-3) and Options' (4-9): vm.cfg's keys ("bootfrom" is
// boot's and args' --root together)
static char *keys[N_KEYS] = { "mb", "mmu", "disk", "cdrom", "system", "bootfrom", "kernel", "boot", "args", "autostart" };
static char *key_label[N_KEYS] = { "Memory (MB)", "MMU (memory protection)", "Hard disk (hd0)", "CD/DVD drive",
                                   "System", "Boots from", "Kernel", "Boot archive", "Kernel arguments", "Start at boot" };
static char logbuf[LOGW + 1];
static int log_at[LOG_MAX];
static char start_log[1024], why_title[64];

static int hv(int c, int a0, int a1, int a2, int a3) {
    *(int *)R_A0 = a0; *(int *)R_A1 = a1; *(int *)R_A2 = a2; *(int *)R_A3 = a3;
    *(int *)R_CMD = c;
    return *(int *)R_RES;
}

static int same(char *a, char *b) { while (*a && *a == *b) { ++a; ++b; } return *a == *b; }

// the value of "KEY value" in TEXT's lines, into OUT (W bytes): 1, or 0
static int line_get(char *text, char *key, char *out, int w) {
    char *p;
    int n, k;
    p = text; n = ui_len(key);
    while (*p) {
        while (*p == ' ' || *p == 9) ++p;
        k = 0; while (k < n && p[k] == key[k]) ++k;
        if (*p != '#' && k == n && (p[n] == ' ' || p[n] == 9 || p[n] == 10 || !p[n])) {
            p = p + n;
            while (*p == ' ' || *p == 9) ++p;
            k = 0;
            while (*p && *p != 10 && *p != 13 && k < w - 1) { out[k] = *p; ++k; ++p; }
            while (k > 0 && (out[k - 1] == ' ' || out[k - 1] == 9)) --k;
            out[k] = 0;
            return 1;
        }
        while (*p && *p != 10) ++p;
        if (*p) ++p;
    }
    out[0] = 0;
    return 0;
}

static char *mname(struct machines *t, int k) { return t->name + k * NAMEW; }

// /vm/NAME/vm.cfg into cfg: its length, or -1
static int cfg_read(char *name) {
    int k;
    upathjoin(path, 128, "/vm", name);
    upathjoin(path, 128, path, "vm.cfg");
    k = uslurp(path, cfg, sizeof cfg - 1);
    if (k < 0) { cfg[0] = 0; return 0 - 1; }
    cfg[k] = 0;
    return k;
}

// KEY's line in /vm/NAME/vm.cfg set to VALUE (or added): 1 if written
static int cfg_set(char *name, char *key, char *value) {
    char *out, *p, *q;
    int n, fd, done, k;
    if (cfg_read(name) < 0) return 0;
    if (!(out = (char *)ualloc(sizeof cfg + 256))) return 0;
    n = ui_len(key);
    p = cfg; q = out; done = 0;
    while (*p) {
        k = 0; while (k < n && p[k] == key[k]) ++k;
        if (k == n && (p[n] == ' ' || p[n] == 9) && !done) {
            ui_cpy(q, key); ui_cat(q, " "); ui_cat(q, value); ui_cat(q, "\n");
            q = q + ui_len(q);
            done = 1;
            while (*p && *p != 10) ++p;
            if (*p) ++p;
        } else {
            while (*p && *p != 10) { *q = *p; ++q; ++p; }
            if (*p) { *q = *p; ++q; ++p; }
        }
    }
    *q = 0;
    if (!done) { if (q > out && q[-1] != 10) ui_cat(out, "\n"); ui_cat(out, key); ui_cat(out, " "); ui_cat(out, value); ui_cat(out, "\n"); }
    if ((fd = uopen(path, O_WR + O_CREATE + O_TRUNCATE)) < 0) { free(out); return 0; }
    write(fd, out, ui_len(out));
    uclose(fd);
    free(out);
    return 1;
}

// the machine called NAME in the list, or a new row for it
static int row_of(struct machines *t, char *name) {
    int k;
    k = 1;
    while (k < t->n) { if (same(mname(t, k), name)) return k; ++k; }
    if (t->n >= MAXM) return 0 - 1;
    k = t->n;
    ++t->n;
    ui_cpy(mname(t, k), name);
    mname(t, k)[NAMEW - 1] = 0;
    t->id[k] = 0; t->state[k] = S_NONE; t->mb[k] = 0; t->cfg[k] = 0; t->cyc[k] = 0; t->rate[k] = 0; t->nhist[k] = 0;
    return k;
}

// "key value" of the hypervisor's info text, as a number
static long long info_num(char *key) {
    long long v;
    char *p;
    line_get(info_buf, key, buf, 40);
    v = 0; p = buf;
    while (*p >= '0' && *p <= '9') { v = v * 10 + *p - '0'; ++p; }
    return v;
}

// The list again: the configured machines, then the made ones; each made
// one's instructions a second since the last look
static void refresh(struct machines *t) {
    int k, id, n, now, dt, st, i;
    long long c;
    char nm[64];
    now = __time();
    dt = t->at_ms ? now - t->at_ms : 0;
    t->at_ms = now;
    k = 1; while (k < t->n) { t->id[k] = 0; t->state[k] = S_NONE; t->cfg[k] = 0; ++k; }
    if (t->n < 1) { t->n = 1; ui_cpy(mname(t, 0), "c4pc"); }
    i = 0;
    while (ureaddir("/vm", i, nm) >= 0) {
        if (nm[0] != '.' && cfg_read(nm) >= 0 && (k = row_of(t, nm)) > 0) {
            t->cfg[k] = 1;
            line_get(cfg, "mb", buf, 16);
            t->mb[k] = buf[0] ? uatoi(buf) : 256;
        }
        ++i;
    }
    n = *(int *)R_COUNT;
    id = 1;
    while (id < n) {
        st = hv(C_STATE, id, 0, 0, 0);
        if (st > 0 && hv(C_INFO, id, (int)info_buf, sizeof info_buf, 0) >= 0) {
            line_get(info_buf, "name", nm, NAMEW);
            if ((k = row_of(t, nm)) > 0) {
                t->id[k] = id; t->state[k] = st;
                t->mb[k] = (int)info_num("mb");
                c = info_num("cycles");
                t->rate[k] = st == S_RUN && dt > 0 && t->cyc[k] && c >= t->cyc[k] ? (int)((c - t->cyc[k]) / dt) : 0;
                t->cyc[k] = c;
            }
        }
        ++id;
    }
    // a machine gone from both (its vm.cfg removed, deleted): out of the list
    k = 1;
    while (k < t->n) {
        if (!t->cfg[k] && !t->id[k]) {
            i = k;
            while (i < t->n - 1) {
                ui_cpy(mname(t, i), mname(t, i + 1));
                t->id[i] = t->id[i + 1]; t->state[i] = t->state[i + 1]; t->mb[i] = t->mb[i + 1]; t->cfg[i] = t->cfg[i + 1];
                t->cyc[i] = t->cyc[i + 1]; t->rate[i] = t->rate[i + 1]; t->nhist[i] = t->nhist[i + 1];
                memcpy((char *)(t->hist + i * HIST), (char *)(t->hist + (i + 1) * HIST), HIST * sizeof(int));
                ++i;
            }
            --t->n;
            if (t->sel >= t->n) t->sel = t->n - 1;
        } else ++k;
    }
    k = 1;
    while (k < t->n) {
        if (t->nhist[k] < HIST) { t->hist[k * HIST + t->nhist[k]] = t->rate[k]; ++t->nhist[k]; }
        else { i = 0; while (i < HIST - 1) { t->hist[k * HIST + i] = t->hist[k * HIST + i + 1]; ++i; } t->hist[k * HIST + HIST - 1] = t->rate[k]; }
        ++k;
    }
}

// the chosen machine's screen into the images, small (Summary) or larger
// (Console), in strips that each fit the display's command ring
static void grab(struct machines *t) {
    int w, h, k, s;
    t->shown = 0;
    if (t->sel < 1 || (t->state[t->sel] != S_RUN && t->state[t->sel] != S_PAUSE) || (t->tab != 0 && t->tab != 1)) return;
    w = t->tab ? CW : TW; h = t->tab ? CH : TH;
    if (hv(C_THUMB, t->id[t->sel], (int)thumb, w, h) < 0) return;
    s = 0; k = 0;
    while (s < h) { d_imgdef(IMG_ID + k, w, STRIP, thumb + s * w); s = s + STRIP; ++k; }
    t->shown = t->id[t->sel];
}

static void say(struct machines *t, char *s) { ui_cpy(t->status, s); }

// `vm ARGS...` in the background: its id, or -1
static int run_vm(struct machines *t, char *a1, char *a2) {
    char *av[4];
    int id;
    if (!vmpath[0] && !desk_which("vm", vmpath)) ui_cpy(vmpath, "/bin/vm");
    av[0] = "vm"; av[1] = a1; av[2] = a2; av[3] = 0;
    id = spawn(vmpath, a2 ? 3 : 2, av);
    return id;
}

static void start_one(struct machines *t, int k) {
    int fd, o1, o2;
    if (k < 1 || k >= t->n || t->state[k] == S_RUN) return;
    if (t->pid) { say(t, "Wait: another machine is being started."); return; }
    if (!t->cfg[k] && t->id[k]) {                       // made by `vm new`: as it was made
        if (hv(C_START, t->id[k], 0, 0, 0) < 0) say(t, "It did not start: is there memory for it?");
        else { ui_cpy(t->status, mname(t, k)); ui_cat(t->status, ": started"); }
        return;
    }
    // from its vm.cfg, as it says now: what was made of it before, gone first
    if (t->id[k]) hv(C_DELETE, t->id[k], 0, 0, 0);
    // what vm up says, into /vm/NAME/start.log (its output and errors)
    upathjoin(path, 128, "/vm", mname(t, k));
    upathjoin(path, 128, path, "start.log");
    fd = uopen(path, O_WR + O_CREATE + O_TRUNCATE);
    if (fd >= 0) { o1 = udup(1); o2 = udup(2); udup2(fd, 1); udup2(fd, 2); }
    t->pid = run_vm(t, "up", mname(t, k));
    if (fd >= 0) { udup2(o1, 1); udup2(o2, 2); uclose(o1); uclose(o2); uclose(fd); }
    if (t->pid < 0) { t->pid = 0; say(t, "vm did not run (is /bin/vm there?)"); return; }
    ui_cpy(t->job, mname(t, k));
    ui_cpy(t->status, mname(t, k)); ui_cat(t->status, ": starting (vm up reads its kernel and boot archive)...");
}

static void stop_one(struct machines *t, int k, int now) {
    if (k < 1 || k >= t->n || t->state[k] != S_RUN) return;
    hv(C_STOP, t->id[k], now, 0, 0);
    ui_cpy(t->status, mname(t, k)); ui_cat(t->status, now ? ": stopped" : ": shutting down (SIGTERM, as the power button)");
}

// Pause, or Resume a paused one
static void pause_one(struct machines *t, int k) {
    if (k < 1 || k >= t->n) return;
    ui_cpy(t->status, mname(t, k));
    if (t->state[k] == S_PAUSE) ui_cat(t->status, hv(C_RESUME, t->id[k], 0, 0, 0) < 0 ? ": not resumed" : ": resumed");
    else if (t->state[k] == S_RUN) ui_cat(t->status, hv(C_PAUSE, t->id[k], 0, 0, 0) < 0 ? ": not paused" : ": paused (Resume goes on)");
    else ui_cat(t->status, ": not running");
}

// Delete: asked first
static void delete_ask(struct win *w, int k) {
    struct machines *t;
    t = S(w);
    if (k < 1 || k >= t->n) return;
    if (t->state[k] == S_RUN || t->state[k] == S_PAUSE) { msgbox(w, mname(t, k), "Stop it first: a machine that runs, or is paused, is not deleted.", 0, A_NONE); return; }
    ui_cpy(why_title, "Delete "); ui_cat(why_title, mname(t, k));
    ui_cpy(start_log, "Delete "); ui_cat(start_log, mname(t, k));
    ui_cat(start_log, "? /vm/"); ui_cat(start_log, mname(t, k));
    ui_cat(start_log, " is removed -- its vm.cfg and its disk images -- and cannot be had back.");
    msgbox(w, why_title, start_log, 1, A_DEL);
}
// the chosen machine gone: out of the hypervisor, /vm/NAME's files, it
static void delete_one(struct machines *t, int k) {
    char dir[96], f[64];
    int n;
    if (k < 1 || k >= t->n || t->state[k] == S_RUN || t->state[k] == S_PAUSE) return;
    if (t->id[k] && hv(C_DELETE, t->id[k], 0, 0, 0) < 0) { say(t, "It could not be taken out of the hypervisor."); return; }
    upathjoin(dir, 96, "/vm", mname(t, k));
    n = 0;
    while (ureaddir(dir, 0, f) >= 0 && n < 1000) {
        upathjoin(path, 128, dir, f);
        if (uunlink(path) < 0) { ui_cpy(t->status, path); ui_cat(t->status, " could not be removed"); return; }
        ++n;
    }
    uunlink(dir);
    __c4_opcode(0, 0, 0, 252);              // sync (SYS_SYNC)
    ui_cpy(t->status, mname(t, k)); ui_cat(t->status, ": deleted");
    refresh(t);
    if (t->sel >= t->n) t->sel = t->n - 1;
}

int machines_open() {
    int i;
    struct win *w;
    w = win_first();
    while (w) { if (w->used && w->kind == K_VMS) { win_raise(w->id); return w->id; } w = w->next; }
    i = win_open(K_VMS, "Machines", 760, 520, sizeof(struct machines));
    if (i < 0) return i;
    w = win_get(i);
    if (!(__c4_info() & C4I_HV_BIT)) say(S(w), "No hypervisor here: GRUB's \"C4PC hypervisor\" entries boot one.");
    else {
        // the hypervisor's window is a device's: the desktop may read it
        // only once it has asked (under the MMU; VMware's clipboard asked
        // already, elsewhere nothing had)
        if (__c4_info() & 0x80000) uioperm(UIOPERM_DEV);
        refresh(S(w)); say(S(w), "Ready.");
    }
    S(w)->sel = S(w)->n > 1 ? 1 : 0;
    mark_dirty();
    return i;
}

// c4pc's log into logbuf, ended by a 0 (C_LOG writes it)
static void log_fetch() { if (hv(C_LOG, (int)logbuf, LOGW + 1, 0, 0) < 0) logbuf[0] = 0; }
// /vm/NAME/start.log, its last 400 bytes from a line's start, into start_log
static void why(char *name) {
    int n, k;
    char *p;
    upathjoin(path, 128, "/vm", name);
    upathjoin(path, 128, path, "start.log");
    start_log[0] = 0;
    if ((n = uslurp(path, logbuf, LOGW)) <= 0) return;
    logbuf[n] = 0;
    while (n > 0 && (logbuf[n - 1] == 10 || logbuf[n - 1] == 13 || logbuf[n - 1] == ' ')) logbuf[--n] = 0;
    k = n > 400 ? n - 400 : 0;
    if (k) { while (logbuf[k] && logbuf[k] != 10) ++k; if (logbuf[k]) ++k; }
    p = logbuf + k;
    k = 0;
    while (*p && k < (int)sizeof start_log - 1) { start_log[k] = *p == 10 ? ' ' : *p; ++k; ++p; }
    start_log[k] = 0;
    // the newlines, kept as "  " between its lines, for the message's wrapping
    ui_cpy(logbuf, "");
}
// the last of the lines in S (newlines turned to spaces by why: its last sentence)
static char *last_line(char *s) {
    char *p, *q;
    p = s; q = s;
    while (*p) { if (p[0] == 'v' && p[1] == 'm' && p[2] == ':' && p > s) q = p; ++p; }
    return q;
}

void machines_tick(struct win *w) {
    struct machines *t;
    int st[2], r, k;
    t = S(w);
    if (!(__c4_info() & C4I_HV_BIT)) return;
    if (t->pid && (r = uwaitpid(t->pid, st, UWNOHANG)) != 0) {
        ui_cpy(t->status, t->job);
        t->pid = 0;
        if (r > 0 && !st[0] && !st[1]) ui_cat(t->status, ": started");
        else {
            // why: what vm up said (start.log), its last lines
            why(t->job);
            ui_cat(t->status, ": did not start: ");
            ui_ncpy(t->status + ui_len(t->status), last_line(start_log), 118 - ui_len(t->status));
            ui_cpy(why_title, t->job); ui_cat(why_title, " did not start");
            msgbox(w, why_title, start_log[0] ? start_log : "vm up said nothing (vm up NAME in a Terminal)", 0, A_NONE);
        }
    }
    if (t->tab == 4) log_fetch();
    refresh(t);
    if (t->restart) {
        k = 1;
        while (k < t->n && t->id[k] != t->restart) ++k;
        if (k >= t->n) t->restart = 0;
        else if (t->state[k] != S_RUN) { t->restart = 0; start_one(t, k); }
    }
    grab(t);
    mark_win_dirty(w);
}

// ---- drawing ----

static void state_text(struct machines *t, int k, char *out) {
    int s;
    s = t->state[k];
    if (k == 0) ui_cpy(out, "the PC (VM 0, this C4IX)");
    else if (s == S_RUN) ui_cpy(out, "running");
    else if (s == S_PAUSE) ui_cpy(out, "paused");
    else if (s == S_HALT) ui_cpy(out, "stopped");
    else if (s == S_MADE) ui_cpy(out, "made, not started");
    else ui_cpy(out, "stopped");
    if (k && t->pid && same(t->job, mname(t, k))) ui_cpy(out, "starting...");
}

// a monitor: its screen green when it runs, amber paused
static void icon(int x, int y, int st) {
    d_rect(x, y + 1, 16, 11, C_DARK);
    d_rect(x + 2, y + 3, 12, 7, st == S_RUN ? 0x30c030 : st == S_PAUSE ? 0xe0a000 : 0x707070);
    d_rect(x + 6, y + 12, 4, 2, C_SHADOW);
    d_rect(x + 3, y + 14, 10, 1, C_SHADOW);
}

static void field(int x, int y, int w, char *label, char *value) {
    d_text(x, y, C_DARK, F_SANS, label);
    d_text(x + 130, y, C_DARK, F_BOLD, value);
}

static void graph(int *h, int n, int x, int y, int w, int ht) {
    int k, max, px, py, qx, qy;
    d_rect(x, y, w, ht, C_DARK);
    k = 1; while (k < 4) { d_rect(x, y + (ht * k) / 4, w, 1, 0x005000); ++k; }
    max = 1; k = 0; while (k < n) { if (h[k] > max) max = h[k]; ++k; }
    if (n < 2) return;
    k = 1;
    while (k < n) {
        px = x + w - 1 - (n - k) * (w - 2) / (HIST - 1);
        qx = x + w - 1 - (n - 1 - k) * (w - 2) / (HIST - 1);
        py = y + ht - 2 - (int)(((long long)h[k - 1] * (ht - 4)) / max);
        qy = y + ht - 2 - (int)(((long long)h[k] * (ht - 4)) / max);
        d_line(px, py, qx, qy, C_GREEN);
        ++k;
    }
}

static void screen_box(struct machines *t, int x, int y, int w, int h) {
    int s, k;
    d_rect(x, y, w, h, C_DARK);
    if (t->sel > 0 && t->shown == t->id[t->sel] && (t->state[t->sel] == S_RUN || t->state[t->sel] == S_PAUSE)) {
        s = 0; k = 0;
        while (s < h) { d_image(IMG_ID + k, x, y + s); s = s + STRIP; ++k; }
    } else d_text(x + w / 2 - 33, y + h / 2 - 6, 0xa0a0a0, F_SANS, "Not running");
}

static void draw_summary(struct machines *t, int x, int y, int w, int h) {
    int k, gy;
    k = t->sel;
    if (k == 0) {
        ui_group(x, y, w, 150, "c4pc");
        field(x + 12, y + 22, w, "Role", "the hypervisor's PC");
        field(x + 12, y + 42, w, "This machine", "VM 0, the control C4IX");
        ui_num(buf, t->n - 1); field(x + 12, y + 62, w, "Machines", buf);
        k = 1; gy = 0; while (k < t->n) { if (t->state[k] == S_RUN) ++gy; ++k; }
        ui_num(buf, gy); field(x + 12, y + 82, w, "Running", buf);
        d_text(x + 12, y + 112, C_DARK, F_SANS, "Create VM makes one; choose it on the left.");
        d_text(x + 12, y + 130, C_DARK, F_SANS, "Log: what c4pc and the machines have said.");
        return;
    }
    ui_group(x, y, w - TW - 24, 190, "Status");
    state_text(t, k, buf); field(x + 12, y + 22, w, "State", buf);
    buf[0] = 0; if (t->id[k]) ui_num(buf, t->id[k]); else ui_cpy(buf, "-");
    field(x + 12, y + 42, w, "ID", buf);
    ui_num(buf, t->mb[k]); ui_cat(buf, " MB"); field(x + 12, y + 62, w, "Memory", buf);
    if (t->state[k] != S_RUN) ui_cpy(buf, "-");
    else if (t->rate[k] >= 1000) { ui_num_commas(buf, t->rate[k] / 1000); ui_cat(buf, " M instructions/s"); }
    else { ui_num(buf, t->rate[k]); ui_cat(buf, " K instructions/s (idle)"); }
    field(x + 12, y + 82, w, "CPU", buf);
    if (t->cfg[k] && cfg_read(mname(t, k)) >= 0) {
        line_get(cfg, "disk", buf, 60); if (!buf[0]) ui_cpy(buf, "none"); field(x + 12, y + 102, w, "Hard disk", buf);
        line_get(cfg, "cdrom", buf, 60); if (!buf[0]) ui_cpy(buf, "none"); else if (same(buf, "pc")) ui_cpy(buf, "the PC's CD drive");
        field(x + 12, y + 122, w, "CD/DVD", buf);
        line_get(cfg, "autostart", buf, 8); field(x + 12, y + 142, w, "Start at boot", same(buf, "yes") ? "yes" : "no");
        line_get(cfg, "system", buf, 30); if (!buf[0]) ui_cpy(buf, "-"); field(x + 12, y + 162, w, "System", buf);
    } else d_text(x + 12, y + 102, C_DARK, F_SANS, "made by vm new: no vm.cfg");
    ui_group(x + w - TW - 16, y, TW + 16, 190, "Screen");
    screen_box(t, x + w - TW - 8, y + 18, TW, TH);
    if (t->state[k] == S_RUN && hv(C_INFO, t->id[k], (int)info_buf, sizeof info_buf, 0) >= 0) {
        line_get(info_buf, "screen", buf, 20);
        d_text(x + w - TW - 8, y + 18 + TH + 6, C_DARK, F_SANS, same(buf, "text") ? "its text console" : "its display");
    }
    gy = y + 200;
    if (h - (gy - y) > 60) {
        ui_group(x, gy, w, h - (gy - y), "CPU: instructions a second, the last minute");
        graph(t->hist + k * HIST, t->nhist[k], x + 8, gy + 18, w - 16, h - (gy - y) - 26);
    }
}

static void draw_console(struct machines *t, int x, int y, int w, int h) {
    if (t->sel == 0) { d_text(x + 8, y + 8, C_DARK, F_SANS, "This is the PC's own screen. Choose a machine."); return; }
    screen_box(t, x + (w - CW) / 2, y + 4, CW, CH);
    ui_button(x + (w - 220) / 2, y + CH + 14, 220, 26, "Attach: its screen, keys, mouse", 0, 1);
    d_text(x + (w - 300) / 2, y + CH + 48, C_DARK, F_SANS, "Ctrl+Alt+Home brings this desktop back.");
}

// "Boots from": the system's archive, or its disk (boot -)
static void bootfrom_text(char *out) {
    line_get(cfg, "boot", out, 70);
    ui_cpy(out, same(out, "-") ? "its disk (--root hd0p1)" : "the system's archive");
}
// Hardware (rows 0-3) and Options (4-9): vm.cfg's lines
static void draw_cfg(struct machines *t, int x, int y, int w, int first, int last) {
    int k, r;
    if (t->sel == 0) { d_text(x + 8, y + 8, C_DARK, F_SANS, "Choose a machine."); return; }
    if (!t->cfg[t->sel] || cfg_read(mname(t, t->sel)) < 0) { d_text(x + 8, y + 8, C_DARK, F_SANS, "This machine has no vm.cfg (vm new made it)."); return; }
    d_rect(x, y, w, (last - first + 1) * ROW_H + 24, C_WHITE);
    d_bevel(x, y, w, (last - first + 1) * ROW_H + 24, B_SUNKEN);
    d_text(x + 8, y + 4, C_DARK, F_BOLD, "Setting");
    d_text(x + 180, y + 4, C_DARK, F_BOLD, "Value");
    r = 0; k = first;
    while (k <= last) {
        line_get(cfg, keys[k], buf, 70);
        if (k == K_BOOTFROM) bootfrom_text(buf);
        else if (!buf[0]) ui_cpy(buf, k == 0 ? "256" : k == 1 ? "yes" : k == 9 ? "no" : k == 2 || k == 3 || k == 7 ? "none" :
                                k == 8 ? "(none: the console)" : "(not said)");
        d_text(x + 8, y + 24 + r * ROW_H, C_DARK, F_SANS, key_label[k]);
        d_text(x + 180, y + 24 + r * ROW_H, C_DARK, F_SANS, buf);
        ++r; ++k;
    }
    d_text(x + 4, y + (last - first + 1) * ROW_H + 34, C_DARK, F_SANS, "A click on a row changes it; the machine has it at its next start.");
}

// does log line P (to its newline) belong to machine NAME: "vmN NAME" in it
static int log_mine(char *p, char *name) {
    int n, k;
    n = ui_len(name);
    while (*p && *p != 10) {
        if (p[0] == 'v' && p[1] == 'm' && p[2] >= '0' && p[2] <= '9') {
            k = 2; while (p[k] >= '0' && p[k] <= '9') ++k;
            if (p[k] == ' ' && !memcmp(p + k + 1, name, n) && (p[k + 1 + n] == ':' || p[k + 1 + n] == ' ' || p[k + 1 + n] == '\'' || p[k + 1 + n] == 10 || !p[k + 1 + n])) return 1;
        }
        ++p;
    }
    return 0;
}
// Log: c4pc's (the PC chosen), or the lines of it that are a machine's,
// the last that fit, a line each
static void draw_log(struct machines *t, int x, int y, int w, int h) {
    int n, k, rows, cols, first, c;
    char *p, line[160];
    d_rect(x, y, w, h, C_DARK);
    if (!logbuf[0]) log_fetch();
    n = 0; p = logbuf;
    while (*p) {
        if (t->sel == 0 || log_mine(p, mname(t, t->sel))) {
            // more lines than are kept: the oldest go
            if (n == LOG_MAX) { k = 0; while (k < LOG_MAX - 1) { log_at[k] = log_at[k + 1]; ++k; } --n; }
            log_at[n] = p - logbuf; ++n;
        }
        while (*p && *p != 10) ++p;
        if (*p) ++p;
    }
    rows = (h - 8) / 14; cols = (w - 8) / 7;
    if (cols > 159) cols = 159;
    first = n > rows ? n - rows : 0;
    if (!n) d_text(x + 6, y + 6, 0xa0a0a0, F_SANS, t->sel ? "Nothing from this machine in c4pc's log yet." : "The log is empty.");
    k = first;
    while (k < n) {
        p = logbuf + log_at[k]; c = 0;
        while (p[c] && p[c] != 10 && c < cols) { line[c] = p[c]; ++c; }
        line[c] = 0;
        d_text(x + 4, y + 4 + (k - first) * 14, 0xd0d0d0, F_MONO, line);
        ++k;
    }
}

void machines_draw(struct win *w, int ox, int oy, int cw, int ch, int focused) {
    struct machines *t;
    int k, y, rx, rw, bx;
    t = S(w);
    tabs[0] = "Summary"; tabs[1] = "Console"; tabs[2] = "Hardware"; tabs[3] = "Options"; tabs[4] = "Log";
    d_rect(ox, oy, cw, ch, C_FACE);
    // the toolbar
    ui_button(ox + 6, oy + 5, 100, 24, "Create VM", 0, 0);
    bx = ox + cw - 6 - N_TB * TB;
    ui_button(bx, oy + 5, TBW, 24, "Start", 0, 0);
    ui_button(bx + TB, oy + 5, TBW, 24, "Shutdown", 0, 0);
    ui_button(bx + 2 * TB, oy + 5, TBW, 24, "Stop", 0, 0);
    ui_button(bx + 3 * TB, oy + 5, TBW, 24, "Restart", 0, 0);
    ui_button(bx + 4 * TB, oy + 5, TBW, 24, t->sel > 0 && t->state[t->sel] == S_PAUSE ? "Resume" : "Pause", 0, 0);
    ui_button(bx + 5 * TB, oy + 5, TBW, 24, "Console", 0, 0);
    ui_button(bx + 6 * TB, oy + 5, TBW, 24, "Delete", 0, 0);
    // the tree
    d_rect(ox + 4, oy + TOOL_H, TREE_W, ch - TOOL_H - 26, C_WHITE);
    d_bevel(ox + 4, oy + TOOL_H, TREE_W, ch - TOOL_H - 26, B_SUNKEN);
    y = oy + TOOL_H + 4;
    k = 0;
    while (k < t->n) {
        if (k == t->sel) d_rect(ox + 6, y - 1, TREE_W - 4, ROW_H - 2, C_SEL);
        if (k == 0) { d_icon(K_VMS, ox + 10, y + 1); d_text(ox + 30, y + 3, k == t->sel ? C_WHITE : C_DARK, F_BOLD, "c4pc"); }
        else {
            d_rect(ox + 14, y + 2, 1, ROW_H - 6, C_SHADOW);
            icon(ox + 22, y + 2, t->state[k]);
            buf[0] = 0;
            if (t->id[k]) { ui_num(buf, t->id[k]); ui_cat(buf, " "); }
            ui_cat(buf, mname(t, k));
            d_text(ox + 44, y + 3, k == t->sel ? C_WHITE : C_DARK, F_SANS, buf);
        }
        y = y + ROW_H;
        ++k;
    }
    // the page
    rx = ox + TREE_W + 10; rw = cw - TREE_W - 14;
    d_bevel(rx, oy + PAGE_Y, rw, ch - PAGE_Y - 26, B_RAISED);
    ui_tabs(rx + 4, oy + TAB_Y, tabs, N_TABS, t->tab);
    if (t->tab == 0) draw_summary(t, rx + 8, oy + PAGE_Y + 10, rw - 16, ch - PAGE_Y - 44);
    else if (t->tab == 1) draw_console(t, rx + 8, oy + PAGE_Y + 8, rw - 16, ch - PAGE_Y - 40);
    else if (t->tab == 2) draw_cfg(t, rx + 8, oy + PAGE_Y + 10, rw - 16, 0, 3);
    else if (t->tab == 3) draw_cfg(t, rx + 8, oy + PAGE_Y + 10, rw - 16, 4, 9);
    else draw_log(t, rx + 8, oy + PAGE_Y + 8, rw - 16, ch - PAGE_Y - 40);
    d_bevel(ox + 2, oy + ch - 22, cw - 4, 20, B_SUNKEN);
    d_text(ox + 8, oy + ch - 18, C_DARK, F_SANS, t->status);
}

// ---- input ----

static void edit_row(struct win *w, int k) {
    struct machines *t;
    t = S(w);
    if (t->sel < 1 || !t->cfg[t->sel] || cfg_read(mname(t, t->sel)) < 0) return;
    if (k == K_SYSTEM) { msgbox(w, mname(t, t->sel), "A machine's system is chosen when it is made (Create VM).", 0, A_NONE); return; }
    if (k == K_BOOTFROM) {
        msgbox(w, mname(t, t->sel), "Boot it from its own disk (once install, in it, has put C4IX on hd0)? No: from its system's archive.", 1, A_EDIT + k);
        return;
    }
    line_get(cfg, keys[k], buf, 120);
    inputbox(w, mname(t, t->sel), key_label[k], buf, A_EDIT + k);
}

// "Boots from": DISK, boot - and --root hd0p1 first in args; or the system's
// archive again, and no --root. 1 if vm.cfg was written
static int set_bootfrom(char *name, int disk) {
    char args[200], out[220], sys[40], *p;
    int k, n0;
    if (cfg_read(name) < 0) return 0;
    line_get(cfg, "args", args, 200);
    line_get(cfg, "system", sys, 40);
    // args less any --root and its word
    out[0] = 0;
    if (disk) ui_cpy(out, "--root hd0p1");
    p = args;
    while (*p) {
        while (*p == ' ') ++p;
        if (!*p) break;
        k = 0; while (p[k] && p[k] != ' ') ++k;
        if (k == 6 && !memcmp(p, "--root", 6)) { p = p + k; while (*p == ' ') ++p; while (*p && *p != ' ') ++p; continue; }
        if (out[0]) ui_cat(out, " ");
        n0 = ui_len(out);
        if (n0 + k < 210) { memcpy(out + n0, p, k); out[n0 + k] = 0; }
        p = p + k;
    }
    if (!cfg_set(name, "args", out)) return 0;
    if (disk) return cfg_set(name, "boot", "-");
    return cfg_set(name, "boot", same(sys, "c4dos") ? "/vm/images/c4dos.tar" : "/vm/images/c4ix.tar");
}

void machines_mouse(struct win *w, int ev, int x, int y, int cw, int ch) {
    struct machines *t;
    int k, rx, rw, bx, first;
    t = S(w);
    if (ev != E_DOWN && ev != E_DBL) return;
    bx = cw - 6 - N_TB * TB;
    rx = TREE_W + 10; rw = cw - TREE_W - 14;
    if (ui_hit(x, y, 6, 5, 100, 24)) { t->nm[0] = 0; inputbox(w, "Create VM", "Name (letters and digits):", "", A_NAME); return; }
    if (t->tab == 4 && t->sel >= 0) logbuf[0] = 0;
    if (y >= 5 && y < 29 && x >= bx) {
        k = (x - bx) / TB;
        if (k == 0) start_one(t, t->sel);
        else if (k == 1) stop_one(t, t->sel, 0);
        else if (k == 2) stop_one(t, t->sel, 1);
        else if (k == 3 && t->sel > 0 && t->state[t->sel] == S_RUN) { t->restart = t->id[t->sel]; stop_one(t, t->sel, 1); }
        else if (k == 3) start_one(t, t->sel);
        else if (k == 4) pause_one(t, t->sel);
        else if (k == 5) t->tab = 1;
        else if (k == 6) delete_ask(w, t->sel);
        return;
    }
    if (ui_hit(x, y, 4, TOOL_H, TREE_W, ch - TOOL_H - 26)) {
        k = (y - TOOL_H - 4) / ROW_H;
        if (k >= 0 && k < t->n) { t->sel = k; t->shown = 0; grab(t); }
        if (ev == E_DBL && k > 0 && k < t->n && t->state[k] == S_RUN) hv(C_ATTACH, t->id[k], 0, 0, 0);
        return;
    }
    k = ui_tab_hit(x, y, rx + 4, TAB_Y, tabs, N_TABS);
    if (k >= 0) { t->tab = k; t->shown = 0; logbuf[0] = 0; grab(t); return; }
    if (t->tab == 1 && t->sel > 0 && ui_hit(x, y, rx + 8 + (rw - 16 - 220) / 2, PAGE_Y + 8 + CH + 14, 220, 26)) {
        if (t->state[t->sel] == S_RUN) hv(C_ATTACH, t->id[t->sel], 0, 0, 0);
        else say(t, "It is not running: Start it first.");
        return;
    }
    if (t->tab == 2 || t->tab == 3) {
        first = t->tab == 2 ? 0 : 4;
        k = (y - PAGE_Y - 10 - 24) / ROW_H;
        if (y >= PAGE_Y + 34 && k >= 0 && k <= (t->tab == 2 ? 3 : 5)) edit_row(w, first + k);
    }
}

void machines_key(struct win *w, int code, int ch, int mods) {
    struct machines *t;
    t = S(w);
    if (code == KEY_UP && t->sel > 0) { --t->sel; t->shown = 0; }
    else if (code == KEY_DOWN && t->sel < t->n - 1) { ++t->sel; t->shown = 0; }
    else if (code == KEY_TAB && (mods & M_CTRL)) { t->tab = (t->tab + 1) % N_TABS; logbuf[0] = 0; }
    else return;
    grab(t);
}

static int good_name(char *s) {
    int k;
    k = 0;
    while (s[k]) {
        if (!((s[k] >= 'a' && s[k] <= 'z') || (s[k] >= 'A' && s[k] <= 'Z') || (s[k] >= '0' && s[k] <= '9') || s[k] == '-' || s[k] == '_')) return 0;
        ++k;
    }
    return k > 0 && k < 24;
}

void machines_answer(struct win *w, int action, int yes) {
    struct machines *t;
    char *s;
    char cmd[160];
    t = S(w);
    if (action == A_DEL) { if (yes) delete_one(t, t->sel); mark_win_dirty(w); return; }
    // Boots from: yes its disk, no its system's archive
    if (action == A_EDIT + K_BOOTFROM && t->sel > 0) {
        if (set_bootfrom(mname(t, t->sel), yes)) {
            ui_cpy(t->status, mname(t, t->sel)); ui_cat(t->status, yes ? ": boots from its disk" : ": boots from its system's archive");
            if (t->state[t->sel] == S_RUN) ui_cat(t->status, ", at its next start");
        } else say(t, "vm.cfg was not written.");
        mark_win_dirty(w);
        return;
    }
    if (!yes) return;
    s = dialog_text();
    if (action == A_NAME) {
        if (!good_name(s)) { msgbox(w, "Create VM", "A name of letters, digits, - and _, up to 23.", 0, A_NONE); return; }
        ui_cpy(t->nm, s);
        inputbox(w, "Create VM", "System: c4ix, c4ix-text, c4ke, c4dos, or an ISO file:", "c4ix", A_SYS);
    } else if (action == A_SYS) {
        if (!s[0] || s[0] == ' ') ui_cpy(s, "c4ix");
        ui_ncpy(t->nsys, s, 127);
        inputbox(w, "Create VM", "Memory (MB):", same(s, "c4ix-text") || same(s, "c4ke") ? "128" : "256", A_MB);
    } else if (action == A_MB) {
        t->nmb = uatoi(s);
        if (t->nmb < 16) t->nmb = 16;
        inputbox(w, "Create VM", "Disk image, MB (0: none; 512+ to install C4IX):", "0", A_DISK);
    } else if (action == A_DISK) {
        t->ndisk = uatoi(s);
        // in a Terminal: the image being written shows there, and any error
        ui_cpy(cmd, "vm create "); ui_cat(cmd, t->nm); ui_cat(cmd, " ");
        ui_num(cmd + ui_len(cmd), t->nmb); ui_cat(cmd, " "); ui_num(cmd + ui_len(cmd), t->ndisk);
        ui_cat(cmd, " "); ui_cat(cmd, t->nsys);
        run_command(cmd);
        ui_cpy(t->status, t->nm); ui_cat(t->status, ": being made in a Terminal; then Start.");
    } else if (action >= A_EDIT && action < A_EDIT + N_KEYS && t->sel > 0) {
        if (cfg_set(mname(t, t->sel), keys[action - A_EDIT], s)) {
            ui_cpy(t->status, mname(t, t->sel)); ui_cat(t->status, ": "); ui_cat(t->status, keys[action - A_EDIT]);
            ui_cat(t->status, t->state[t->sel] == S_RUN ? " set, for its next start" : " set");
        } else say(t, "vm.cfg was not written.");
    }
    mark_win_dirty(w);
}

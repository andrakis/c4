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
// until Ctrl+Alt+Home), Hardware and Options (vm.cfg, a row each: a click
// edits it, for the next start). Create VM asks a name, the memory, a
// new disk image's size and the CD (the C4PC disc, or an ISO file), and
// runs `vm create` in a Terminal, which shows the image being written.
//
// The machine manager's window (HV_BASE) is read directly: state, info,
// start, stop, attach and THUMB, its screen scaled into our memory. What
// is slow -- `vm up`, which reads the kernel and the boot archive, and
// `vm create` -- runs as `vm`, in the background or in a Terminal.

#include "desktop.h"

enum { R_A0 = 0x564, R_A1 = 0x568, R_A2 = 0x56c, R_A3 = 0x570, R_CMD = 0x574, R_RES = 0x578, R_COUNT = 0x57c };
enum { C_START = 5, C_STOP = 6, C_ATTACH = 7, C_STATE = 9, C_INFO = 10, C_DELETE = 11, C_THUMB = 16 };
enum { S_FREE = 0, S_MADE = 1, S_RUN = 2, S_HALT = 3, S_NONE = 9 };
enum { MAXM = 16, NAMEW = 32, HIST = 60, TREE_W = 160, TOOL_H = 34, TAB_Y = 40, PAGE_Y = 62, ROW_H = 22,
       TW = 192, TH = 144, CW = 384, CH = 288, STRIP = 36, IMG_ID = 300,
       A_NAME = 40, A_MB = 41, A_DISK = 42, A_ISO = 43, A_EDIT = 50, N_KEYS = 7 };

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
};

static struct machines *S(struct win *w) { return (struct machines *)w->st; }
static char buf[160], info_buf[512], cfg[2048], path[128], vmpath[96];
static int thumb[CW * CH];
static char *tabs[4];
static char *keys[N_KEYS] = { "mb", "disk", "cdrom", "kernel", "boot", "args", "autostart" };
static char *key_label[N_KEYS] = { "Memory (MB)", "Hard disk (hd0)", "CD/DVD drive", "Kernel", "Boot archive",
                                   "Kernel arguments", "Start at boot" };

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
    if (t->sel < 1 || t->state[t->sel] != S_RUN || (t->tab != 0 && t->tab != 1)) return;
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
    if (k < 1 || k >= t->n || t->state[k] == S_RUN) return;
    if (t->pid) { say(t, "Wait: another machine is being started."); return; }
    if (!t->cfg[k] && t->id[k]) {                       // made by `vm new`: as it was made
        if (hv(C_START, t->id[k], 0, 0, 0) < 0) say(t, "It did not start: is there memory for it?");
        else { ui_cpy(t->status, mname(t, k)); ui_cat(t->status, ": started"); }
        return;
    }
    // from its vm.cfg, as it says now: what was made of it before, gone first
    if (t->id[k]) hv(C_DELETE, t->id[k], 0, 0, 0);
    if ((t->pid = run_vm(t, "up", mname(t, k))) < 0) { t->pid = 0; say(t, "vm did not run (is /bin/vm there?)"); return; }
    ui_cpy(t->job, mname(t, k));
    ui_cpy(t->status, mname(t, k)); ui_cat(t->status, ": starting (vm up reads its kernel and boot archive)...");
}

static void stop_one(struct machines *t, int k, int now) {
    if (k < 1 || k >= t->n || t->state[k] != S_RUN) return;
    hv(C_STOP, t->id[k], now, 0, 0);
    ui_cpy(t->status, mname(t, k)); ui_cat(t->status, now ? ": stopped" : ": shutting down (SIGTERM, as the power button)");
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
    else { refresh(S(w)); say(S(w), "Ready."); }
    S(w)->sel = S(w)->n > 1 ? 1 : 0;
    mark_dirty();
    return i;
}

void machines_tick(struct win *w) {
    struct machines *t;
    int st[2], r, k;
    t = S(w);
    if (!(__c4_info() & C4I_HV_BIT)) return;
    if (t->pid && (r = uwaitpid(t->pid, st, UWNOHANG)) != 0) {
        ui_cpy(t->status, t->job);
        if (r > 0 && !st[0] && !st[1]) ui_cat(t->status, ": started");
        else ui_cat(t->status, ": did not start -- run vm up in a Terminal to see why");
        t->pid = 0;
    }
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
    else if (s == S_HALT) ui_cpy(out, "stopped");
    else if (s == S_MADE) ui_cpy(out, "made, not started");
    else ui_cpy(out, "stopped");
    if (k && t->pid && same(t->job, mname(t, k))) ui_cpy(out, "starting...");
}

// a monitor: its screen green when it runs
static void icon(int x, int y, int on) {
    d_rect(x, y + 1, 16, 11, C_DARK);
    d_rect(x + 2, y + 3, 12, 7, on ? 0x30c030 : 0x707070);
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
    if (t->sel > 0 && t->shown == t->id[t->sel] && t->state[t->sel] == S_RUN) {
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
        return;
    }
    ui_group(x, y, w - TW - 24, 170, "Status");
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
    } else d_text(x + 12, y + 102, C_DARK, F_SANS, "made by vm new: no vm.cfg");
    ui_group(x + w - TW - 16, y, TW + 16, 170, "Screen");
    screen_box(t, x + w - TW - 8, y + 18, TW, TH);
    gy = y + 180;
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

// Hardware (rows 0-2) and Options (3-6): vm.cfg's lines
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
        if (!buf[0]) ui_cpy(buf, k == 0 ? "256" : k == 6 ? "no" : k == 4 || k == 2 || k == 1 ? "none" : "(default)");
        d_text(x + 8, y + 24 + r * ROW_H, C_DARK, F_SANS, key_label[k]);
        d_text(x + 180, y + 24 + r * ROW_H, C_DARK, F_SANS, buf);
        ++r; ++k;
    }
    d_text(x + 4, y + (last - first + 1) * ROW_H + 34, C_DARK, F_SANS, "A click on a row changes it; the machine has it at its next start.");
}

void machines_draw(struct win *w, int ox, int oy, int cw, int ch, int focused) {
    struct machines *t;
    int k, y, rx, rw, bx;
    t = S(w);
    tabs[0] = "Summary"; tabs[1] = "Console"; tabs[2] = "Hardware"; tabs[3] = "Options";
    d_rect(ox, oy, cw, ch, C_FACE);
    // the toolbar
    ui_button(ox + 6, oy + 5, 100, 24, "Create VM", 0, 0);
    bx = ox + cw - 6 - 5 * 84;
    ui_button(bx, oy + 5, 80, 24, "Start", 0, 0);
    ui_button(bx + 84, oy + 5, 80, 24, "Shutdown", 0, 0);
    ui_button(bx + 168, oy + 5, 80, 24, "Stop", 0, 0);
    ui_button(bx + 252, oy + 5, 80, 24, "Restart", 0, 0);
    ui_button(bx + 336, oy + 5, 80, 24, "Console", 0, 0);
    // the tree
    d_rect(ox + 4, oy + TOOL_H, TREE_W, ch - TOOL_H - 26, C_WHITE);
    d_bevel(ox + 4, oy + TOOL_H, TREE_W, ch - TOOL_H - 26, B_SUNKEN);
    y = oy + TOOL_H + 4;
    k = 0;
    while (k < t->n) {
        if (k == t->sel) d_rect(ox + 6, y - 1, TREE_W - 4, ROW_H - 2, C_SEL);
        if (k == 0) { d_icon(ICON_TASKS, ox + 10, y + 1); d_text(ox + 30, y + 3, k == t->sel ? C_WHITE : C_DARK, F_BOLD, "c4pc"); }
        else {
            d_rect(ox + 14, y + 2, 1, ROW_H - 6, C_SHADOW);
            icon(ox + 22, y + 2, t->state[k] == S_RUN);
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
    ui_tabs(rx + 4, oy + TAB_Y, tabs, 4, t->tab);
    if (t->tab == 0) draw_summary(t, rx + 8, oy + PAGE_Y + 10, rw - 16, ch - PAGE_Y - 44);
    else if (t->tab == 1) draw_console(t, rx + 8, oy + PAGE_Y + 8, rw - 16, ch - PAGE_Y - 40);
    else if (t->tab == 2) draw_cfg(t, rx + 8, oy + PAGE_Y + 10, rw - 16, 0, 2);
    else draw_cfg(t, rx + 8, oy + PAGE_Y + 10, rw - 16, 3, 6);
    d_bevel(ox + 2, oy + ch - 22, cw - 4, 20, B_SUNKEN);
    d_text(ox + 8, oy + ch - 18, C_DARK, F_SANS, t->status);
}

// ---- input ----

static void edit_row(struct win *w, int k) {
    struct machines *t;
    t = S(w);
    if (t->sel < 1 || !t->cfg[t->sel] || cfg_read(mname(t, t->sel)) < 0) return;
    line_get(cfg, keys[k], buf, 120);
    inputbox(w, mname(t, t->sel), key_label[k], buf, A_EDIT + k);
}

void machines_mouse(struct win *w, int ev, int x, int y, int cw, int ch) {
    struct machines *t;
    int k, rx, rw, bx, first;
    t = S(w);
    if (ev != E_DOWN && ev != E_DBL) return;
    bx = cw - 6 - 5 * 84;
    rx = TREE_W + 10; rw = cw - TREE_W - 14;
    if (ui_hit(x, y, 6, 5, 100, 24)) { t->nm[0] = 0; inputbox(w, "Create VM", "Name (letters and digits):", "", A_NAME); return; }
    if (y >= 5 && y < 29 && x >= bx) {
        k = (x - bx) / 84;
        if (k == 0) start_one(t, t->sel);
        else if (k == 1) stop_one(t, t->sel, 0);
        else if (k == 2) stop_one(t, t->sel, 1);
        else if (k == 3 && t->sel > 0 && t->state[t->sel] == S_RUN) { t->restart = t->id[t->sel]; stop_one(t, t->sel, 1); }
        else if (k == 3) start_one(t, t->sel);
        else if (k == 4) t->tab = 1;
        return;
    }
    if (ui_hit(x, y, 4, TOOL_H, TREE_W, ch - TOOL_H - 26)) {
        k = (y - TOOL_H - 4) / ROW_H;
        if (k >= 0 && k < t->n) { t->sel = k; t->shown = 0; grab(t); }
        if (ev == E_DBL && k > 0 && k < t->n && t->state[k] == S_RUN) hv(C_ATTACH, t->id[k], 0, 0, 0);
        return;
    }
    k = ui_tab_hit(x, y, rx + 4, TAB_Y, tabs, 4);
    if (k >= 0) { t->tab = k; t->shown = 0; grab(t); return; }
    if (t->tab == 1 && t->sel > 0 && ui_hit(x, y, rx + 8 + (rw - 16 - 220) / 2, PAGE_Y + 8 + CH + 14, 220, 26)) {
        if (t->state[t->sel] == S_RUN) hv(C_ATTACH, t->id[t->sel], 0, 0, 0);
        else say(t, "It is not running: Start it first.");
        return;
    }
    if (t->tab == 2 || t->tab == 3) {
        first = t->tab == 2 ? 0 : 3;
        k = (y - PAGE_Y - 10 - 24) / ROW_H;
        if (y >= PAGE_Y + 34 && k >= 0 && k <= (t->tab == 2 ? 2 : 3)) edit_row(w, first + k);
    }
}

void machines_key(struct win *w, int code, int ch, int mods) {
    struct machines *t;
    t = S(w);
    if (code == KEY_UP && t->sel > 0) { --t->sel; t->shown = 0; }
    else if (code == KEY_DOWN && t->sel < t->n - 1) { ++t->sel; t->shown = 0; }
    else if (code == KEY_TAB && (mods & M_CTRL)) t->tab = (t->tab + 1) % 4;
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
    if (!yes) return;
    s = dialog_text();
    if (action == A_NAME) {
        if (!good_name(s)) { msgbox(w, "Create VM", "A name of letters, digits, - and _, up to 23.", 0, A_NONE); return; }
        ui_cpy(t->nm, s);
        inputbox(w, "Create VM", "Memory (MB):", "256", A_MB);
    } else if (action == A_MB) {
        t->nmb = uatoi(s);
        if (t->nmb < 16) t->nmb = 16;
        inputbox(w, "Create VM", "A new disk image, its size in MB:", "1024", A_DISK);
    } else if (action == A_DISK) {
        t->ndisk = uatoi(s);
        inputbox(w, "Create VM", "CD: pc (the C4PC disc), or an ISO file's path:", "pc", A_ISO);
    } else if (action == A_ISO) {
        // in a Terminal: the image being written shows there, and any error
        ui_cpy(cmd, "vm create "); ui_cat(cmd, t->nm); ui_cat(cmd, " ");
        ui_num(cmd + ui_len(cmd), t->nmb); ui_cat(cmd, " "); ui_num(cmd + ui_len(cmd), t->ndisk);
        if (s[0] && !same(s, "pc")) { ui_cat(cmd, " "); ui_cat(cmd, s); }
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

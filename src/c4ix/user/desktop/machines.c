// machines.c -- Machines: the hypervisor's machines, as Proxmox shows them
// (c4pc --hv: docs/c4pc-hypervisor.md, stage 3). The control C4IX's
// desktop has it in the Go menu and on the desktop when the hypervisor is
// there, and opens it at its start.
//
// A tree on the left: c4pc, the PC; under it the control C4IX (VM 0, this
// one: shown, not changed) and every machine -- each /vm/NAME/vm.cfg, and
// each the hypervisor has made. On the right the one chosen: a toolbar
// (Start, Shutdown, Stop, Restart, Pause or Resume, Attach, Delete) and
// tabs:
//   Summary   its state and settings, its CPU a second, and its screen large
//   Console   its screen, and Attach: the PC's screen, keyboard and mouse
//             until Ctrl+Alt+Home
//   Hardware  memory, CPUs, MMU, its hard disk, swap and CD images; for
//             c4pc, what c4pc found of the PC (HOST)
//   Options   what it boots from, its kernel, boot archive and arguments,
//             start at boot
//   Load      each of its CPUs' instructions a second (c4pc: CPU 0's time
//             busy), the last minute
//   Log       its console, as c4pc keeps it (LOG: c4pc's own, the control
//             C4IX's, each guest's), scrolled
// Hardware and Options are a form: dropdowns where there is a choice,
// fields that take only what fits (red-edged otherwise), Browse... for a
// file; Apply writes vm.cfg, for the machine's next start. Create VM is one
// form as well, and runs `vm create` in a Terminal, which shows the disk
// image being written. A start that fails says why: `vm up`'s words, kept
// in /vm/NAME/start.log. Shut Down and Restart from the Go menu, with
// machines running, have them shut down here first (machines_before_power).
//
// The machine manager's window (HV_BASE) is read directly: state, info,
// start, stop, attach, THUMB (a screen scaled into our memory), LOG, HOST.
// What is slow -- `vm up`, which reads the kernel and the boot archive, and
// `vm create` -- runs as `vm`, in the background or in a Terminal.

#include "desktop.h"

enum { R_A0 = 0x564, R_A1 = 0x568, R_A2 = 0x56c, R_A3 = 0x570, R_CMD = 0x574, R_RES = 0x578, R_COUNT = 0x57c };
enum { C_START = 5, C_STOP = 6, C_ATTACH = 7, C_STATE = 9, C_INFO = 10, C_DELETE = 11, C_THUMB = 16, C_LOG = 17,
       C_PAUSE = 18, C_RESUME = 19, C_HOST = 21 };
enum { S_FREE = 0, S_MADE = 1, S_RUN = 2, S_HALT = 3, S_PAUSE = 4, S_NONE = 9 };
// the toolbar: Create VM, then seven buttons 74 wide, 78 apart, to the right
enum { N_TB = 7, TB = 78, TBW = 74 };
enum { MAXM = 16, NAMEW = 32, HIST = 60, MAXC = 8, TREE_W = 170, TOOL_H = 34, TAB_Y = 40, PAGE_Y = 62, ROW_H = 22,
       SW_MAX = 640, SH_MAX = 504, STRIP = 36, IMG_ID = 300, N_TABS = 6, LOGW = 32768, LOG_MAX = 800, LROW = 14,
       CTL = 1,                                   // the control C4IX's row
       A_DEL = 45, A_BOOT = 46, A_BROWSE = 60, A_CBROWSE = 80,
       N_F = 11, FROW = 30, FLX = 170 };
// the form's fields (Hardware 0-5, Options 6-10) and Create VM's (C_*)
enum { F_MB = 0, F_CPUS = 1, F_MMU = 2, F_DISK = 3, F_SWAP = 4, F_CD = 5, F_BOOT = 6, F_KERNEL = 7, F_ARCH = 8,
       F_ARGS = 9, F_AUTO = 10 };
enum { CF_NAME = 0, CF_SYS = 1, CF_ISO = 2, CF_MB = 3, CF_CPUS = 4, CF_DISK = 5, CF_SWAP = 6, CF_AUTO = 7, N_CF = 8 };

struct machines {
    int n, sel, tab;                     // sel: 0 c4pc, 1 the control C4IX, 2.. a machine
    char name[MAXM * NAMEW];
    int id[MAXM], state[MAXM], mb[MAXM], cfg[MAXM], ncpu[MAXM];
    long long cyc[MAXM];
    long long ccyc[MAXM * MAXC];         // each CPU's instructions, at the last look
    int rate[MAXM];                      // thousands of instructions a second
    int hist[MAXM * HIST], nhist[MAXM];
    int chist[MAXM * MAXC * HIST], nchist[MAXM];   // each CPU's, the last minute (row 0: c4pc's CPU 0 busy, %)
    long long up0, idle0;                // c4pc's CPU 0: up and idle, ms, at the last look
    int at_ms;
    int pid;                             // a `vm up` running, or 0
    char job[NAMEW];
    int restart;                         // a machine to start again once it has stopped: its id, or 0
    char status[120];
    int shown, sw, sh;                   // whose screen the images are (its row), and their size
    int power;                           // shutting the machines down before C4IX: since when (ms), or 0
    char power_cmd[16];                  // ... then this: shutdown, or reboot
    int ltop;                            // the Log's first line shown; -1: its end, following
    int lrows, ln;                       // its rows shown, and lines
    // the form (Hardware and Options): its row's vm.cfg, as being changed
    int form_row;                        // whose it is (-1 none)
    char fv[N_F * 128];
    int fcaret[N_F], focus;              // the field with the keyboard (-1 none)
    struct ui_drop dcpu, dmmu, dboot;
    int fauto;
    // Create VM
    int create;                          // its form is shown
    char cv[N_CF * 128];
    int ccaret[N_CF], cfocus;
    struct ui_drop csys, ccpus;
    int cauto, cmb_said;                 // Start at boot; the memory typed (not the system's)
};

static struct machines *S(struct win *w) { return (struct machines *)w->st; }
static char buf[200], info_buf[600], host_buf[2048], cfg[2048], path[128], vmpath[96], browse[WK_PATH];
static int thumb[SW_MAX * SH_MAX];
static char *tabs[N_TABS];
static char logbuf[LOGW + 1];
static int log_at[LOG_MAX];
static char start_log[1024], why_title[64];
static char *cpu_items[4] = { "1", "2", "4", "8" };
static char *yn_items[2] = { "yes", "no" };
static char *boot_items[2] = { "its system's archive", "its disk (after install in it)" };
static char *sys_items[5] = { "C4IX, its desktop", "C4IX, its text console", "C4KE, at its shell", "C4DOS, grin and the games",
                              "C4IX, an ISO file as its CD" };
static char *sys_names[5] = { "c4ix", "c4ix-text", "c4ke", "c4dos", "iso" };
static int sys_mb[5] = { 256, 128, 128, 256, 256 };
static char *f_label[N_F] = { "Memory (MB)", "CPUs", "MMU (memory protection)", "Hard disk image", "Swap image",
                              "CD/DVD (an ISO, pc, or none)", "Boots from", "Kernel", "Boot archive", "Kernel arguments", "Start at boot" };
static char *c_label[N_CF] = { "Name", "System", "ISO file", "Memory (MB)", "CPUs", "Hard disk image (MB, 0: none)",
                               "Swap image (MB, 0: none)", "Start at boot" };

static int hv(int c, int a0, int a1, int a2, int a3) {
    *(int *)R_A0 = a0; *(int *)R_A1 = a1; *(int *)R_A2 = a2; *(int *)R_A3 = a3;
    *(int *)R_CMD = c;
    return *(int *)R_RES;
}

static int same(char *a, char *b) { while (*a && *a == *b) { ++a; ++b; } return *a == *b; }
static char *fv(struct machines *t, int k) { return t->fv + k * 128; }
static char *cv(struct machines *t, int k) { return t->cv + k * 128; }
static char *mname(struct machines *t, int k) { return t->name + k * NAMEW; }
static int guest(struct machines *t, int k) { return k > CTL && k < t->n; }
static int up(struct machines *t, int k) { return t->state[k] == S_RUN || t->state[k] == S_PAUSE; }

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
static long long num_of(char *s) {
    long long v;
    v = 0;
    while (*s >= '0' && *s <= '9') { v = v * 10 + *s - '0'; ++s; }
    return v;
}
// "key value" of the hypervisor's info text, as a number
static long long info_num(char *key) { line_get(info_buf, key, buf, 40); return num_of(buf); }

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

// KEY's line in /vm/NAME/vm.cfg set to VALUE (or added; taken out for an
// empty VALUE): 1 if written
static int cfg_set(char *name, char *key, char *value) {
    char *out, *p, *q;
    int n, fd, done, k;
    if (cfg_read(name) < 0) return 0;
    if (!(out = (char *)ualloc(sizeof cfg + 256))) return 0;
    n = ui_len(key);
    p = cfg; q = out; done = 0;
    while (*p) {
        k = 0; while (k < n && p[k] == key[k]) ++k;
        if (k == n && (p[n] == ' ' || p[n] == 9 || p[n] == 10) && !done) {
            if (value[0]) { ui_cpy(q, key); ui_cat(q, " "); ui_cat(q, value); ui_cat(q, "\n"); }
            else *q = 0;
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
    if (!done && value[0]) { if (q > out && q[-1] != 10) ui_cat(out, "\n"); ui_cat(out, key); ui_cat(out, " "); ui_cat(out, value); ui_cat(out, "\n"); }
    if ((fd = uopen(path, O_WR + O_CREATE + O_TRUNCATE)) < 0) { free(out); return 0; }
    write(fd, out, ui_len(out));
    uclose(fd);
    free(out);
    return 1;
}

// the machine called NAME in the list, or a new row for it
static int row_of(struct machines *t, char *name) {
    int k;
    k = CTL + 1;
    while (k < t->n) { if (same(mname(t, k), name)) return k; ++k; }
    if (t->n >= MAXM) return 0 - 1;
    k = t->n;
    ++t->n;
    ui_cpy(mname(t, k), name);
    mname(t, k)[NAMEW - 1] = 0;
    t->id[k] = 0; t->state[k] = S_NONE; t->mb[k] = 0; t->cfg[k] = 0; t->cyc[k] = 0; t->rate[k] = 0;
    t->nhist[k] = 0; t->nchist[k] = 0; t->ncpu[k] = 1;
    return k;
}

// a value onto history H (N so far, HIST at most)
static void hist_add(int *h, int *n, int v) {
    int i;
    if (*n < HIST) { h[*n] = v; ++*n; return; }
    i = 0; while (i < HIST - 1) { h[i] = h[i + 1]; ++i; }
    h[HIST - 1] = v;
}
// where row K's CPUs' next values go in their histories: the next place,
// or, the minute full, the last, everything moved back one first
static int cpus_slot(struct machines *t, int k) {
    int c, i;
    if (t->nchist[k] < HIST) return t->nchist[k];
    c = 0;
    while (c < MAXC) {
        i = 0;
        while (i < HIST - 1) { t->chist[(k * MAXC + c) * HIST + i] = t->chist[(k * MAXC + c) * HIST + i + 1]; ++i; }
        ++c;
    }
    t->nchist[k] = HIST - 1;
    return HIST - 1;
}
// row K's CPUs from info_buf: their instructions since the last look, a
// second, onto their histories
static void cpus_from_info(struct machines *t, int k, int dt) {
    int c, nc, at;
    long long v;
    at = cpus_slot(t, k);
    nc = 0; c = 0;
    while (c < MAXC) {
        ui_cpy(buf, "cpu"); ui_num(buf + 3, c);
        line_get(info_buf, buf, buf + 100, 40);
        if (!buf[100]) break;
        v = num_of(buf + 100);
        if (dt > 0 && t->ccyc[k * MAXC + c] && v >= t->ccyc[k * MAXC + c])
            t->chist[(k * MAXC + c) * HIST + at] = (int)((v - t->ccyc[k * MAXC + c]) / dt);
        else t->chist[(k * MAXC + c) * HIST + at] = 0;
        t->ccyc[k * MAXC + c] = v;
        ++c; ++nc;
    }
    t->ncpu[k] = nc ? nc : 1;
    ++t->nchist[k];
}

// The list again: the configured machines, then the made ones; each made
// one's instructions a second since the last look, and its CPUs'; the
// control C4IX's; c4pc's CPU 0's time busy
static void refresh(struct machines *t) {
    int k, id, n, now, dt, st, i;
    long long c, up0, idle0;
    char nm[64];
    now = __time();
    dt = t->at_ms ? now - t->at_ms : 0;
    t->at_ms = now;
    if (t->n < 2) {
        t->n = 2;
        ui_cpy(mname(t, 0), "c4pc"); ui_cpy(mname(t, CTL), "control");
        t->ncpu[0] = 1; t->ncpu[CTL] = 1; t->state[CTL] = S_RUN; t->id[CTL] = 0;
    }
    k = CTL + 1; while (k < t->n) { t->id[k] = 0; t->state[k] = S_NONE; t->cfg[k] = 0; ++k; }
    i = 0;
    while (ureaddir("/vm", i, nm) >= 0) {
        if (nm[0] != '.' && cfg_read(nm) >= 0 && (k = row_of(t, nm)) > CTL) {
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
            if ((k = row_of(t, nm)) > CTL) {
                t->id[k] = id; t->state[k] = st;
                t->mb[k] = (int)info_num("mb");
                c = info_num("cycles");
                t->rate[k] = st == S_RUN && dt > 0 && t->cyc[k] && c >= t->cyc[k] ? (int)((c - t->cyc[k]) / dt) : 0;
                t->cyc[k] = c;
                if (st == S_RUN) cpus_from_info(t, k, dt);
            }
        }
        ++id;
    }
    // the control C4IX: its CPUs
    if (hv(C_INFO, 0, (int)info_buf, sizeof info_buf, 0) >= 0) {
        t->mb[CTL] = (int)info_num("mb");
        cpus_from_info(t, CTL, dt);
        c = 0; i = 0; while (i < t->ncpu[CTL]) { c = c + t->ccyc[CTL * MAXC + i]; ++i; }
        t->rate[CTL] = dt > 0 && t->cyc[CTL] && c >= t->cyc[CTL] ? (int)((c - t->cyc[CTL]) / dt) : 0;
        t->cyc[CTL] = c;
    }
    // c4pc's CPU 0: the share of the time it was busy
    if (hv(C_HOST, (int)host_buf, sizeof host_buf, 0, 0) >= 0) {
        line_get(host_buf, "uptime", buf, 30); up0 = num_of(buf);
        line_get(host_buf, "idle", buf, 30); idle0 = num_of(buf);
        i = cpus_slot(t, 0);
        t->chist[i] = t->up0 && up0 > t->up0 ? (int)(100 - ((idle0 - t->idle0) * 100) / (up0 - t->up0)) : 0;
        ++t->nchist[0];
        t->up0 = up0; t->idle0 = idle0;
    } else host_buf[0] = 0;
    // a machine gone from both (its vm.cfg removed, deleted): out of the list
    k = CTL + 1;
    while (k < t->n) {
        if (!t->cfg[k] && !t->id[k]) {
            i = k;
            while (i < t->n - 1) {
                ui_cpy(mname(t, i), mname(t, i + 1));
                t->id[i] = t->id[i + 1]; t->state[i] = t->state[i + 1]; t->mb[i] = t->mb[i + 1]; t->cfg[i] = t->cfg[i + 1];
                t->cyc[i] = t->cyc[i + 1]; t->rate[i] = t->rate[i + 1]; t->nhist[i] = t->nhist[i + 1];
                t->ncpu[i] = t->ncpu[i + 1]; t->nchist[i] = t->nchist[i + 1];
                memcpy((char *)(t->hist + i * HIST), (char *)(t->hist + (i + 1) * HIST), HIST * sizeof(int));
                memcpy((char *)(t->chist + i * MAXC * HIST), (char *)(t->chist + (i + 1) * MAXC * HIST), MAXC * HIST * sizeof(int));
                memcpy((char *)(t->ccyc + i * MAXC), (char *)(t->ccyc + (i + 1) * MAXC), MAXC * sizeof(long long));
                ++i;
            }
            --t->n;
            if (t->sel >= t->n) t->sel = t->n - 1;
            if (t->form_row >= k) t->form_row = 0 - 1;
        } else ++k;
    }
    k = 1;
    while (k < t->n) { hist_add(t->hist + k * HIST, t->nhist + k, t->rate[k]); ++k; }
}

// the chosen machine's screen into the images, at the size the page last
// had for it (Summary's or Console's), in strips that each fit the
// display's command ring
static void grab(struct machines *t) {
    int k, s, id;
    t->shown = 0 - 1;
    if (t->sel < CTL || t->tab > 1 || t->sw < 32 || t->sh < STRIP) return;
    if (t->sel > CTL && !up(t, t->sel)) return;
    id = t->sel == CTL ? 0 : t->id[t->sel];
    if (hv(C_THUMB, id, (int)thumb, t->sw, t->sh) < 0) return;
    s = 0; k = 0;
    while (s < t->sh) { d_imgdef(IMG_ID + k, t->sw, STRIP, thumb + s * t->sw); s = s + STRIP; ++k; }
    t->shown = t->sel;
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

static int not_guest(struct machines *t, int k) {
    if (k == 0) { say(t, "c4pc is the PC: there is nothing to do to it here."); return 1; }
    if (k == CTL) { say(t, "The control C4IX is this one: Go > Shut Down or Restart."); return 1; }
    return k >= t->n;
}

static void start_one(struct machines *t, int k) {
    int fd, o1, o2;
    if (not_guest(t, k) || up(t, k)) return;
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
    if (not_guest(t, k) || !up(t, k)) return;
    hv(C_STOP, t->id[k], now, 0, 0);
    ui_cpy(t->status, mname(t, k)); ui_cat(t->status, now ? ": stopped" : ": shutting down (SIGTERM, as the power button)");
}

// Pause, or Resume a paused one
static void pause_one(struct machines *t, int k) {
    if (not_guest(t, k)) return;
    ui_cpy(t->status, mname(t, k));
    if (t->state[k] == S_PAUSE) ui_cat(t->status, hv(C_RESUME, t->id[k], 0, 0, 0) < 0 ? ": not resumed" : ": resumed");
    else if (t->state[k] == S_RUN) ui_cat(t->status, hv(C_PAUSE, t->id[k], 0, 0, 0) < 0 ? ": not paused" : ": paused (Resume goes on)");
    else ui_cat(t->status, ": not running");
}
static void attach_one(struct machines *t, int k) {
    if (not_guest(t, k)) return;
    if (t->state[k] == S_RUN) hv(C_ATTACH, t->id[k], 0, 0, 0);
    else say(t, t->state[k] == S_PAUSE ? "It is paused: Resume it first." : "It is not running: Start it first.");
}

// Delete: asked first
static void delete_ask(struct win *w, int k) {
    struct machines *t;
    t = S(w);
    if (not_guest(t, k)) return;
    if (up(t, k)) { msgbox(w, mname(t, k), "Stop it first: a machine that runs, or is paused, is not deleted.", 0, A_NONE); return; }
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
    if (not_guest(t, k) || up(t, k)) return;
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
    t->form_row = 0 - 1;
    refresh(t);
    if (t->sel >= t->n) t->sel = t->n - 1;
}

// ---- the form: Hardware and Options ----

static int drop_of(struct ui_drop *d, char *v, char **items, int n) {
    int k;
    d->items = items; d->n = n; d->open = 0; d->hover = 0; d->sel = 0;
    k = 0; while (k < n) { if (same(items[k], v)) d->sel = k; ++k; }
    return d->sel;
}
// row K's vm.cfg into the form
static void form_load(struct machines *t, int k) {
    int i;
    t->form_row = k; t->focus = 0 - 1;
    i = 0; while (i < N_F) { fv(t, i)[0] = 0; t->fcaret[i] = 0; ++i; }
    if (!guest(t, k) || !t->cfg[k] || cfg_read(mname(t, k)) < 0) return;
    line_get(cfg, "mb", fv(t, F_MB), 16); if (!fv(t, F_MB)[0]) ui_cpy(fv(t, F_MB), "256");
    line_get(cfg, "cpus", buf, 8); if (!buf[0]) ui_cpy(buf, "1");
    drop_of(&t->dcpu, buf, cpu_items, 4);
    line_get(cfg, "mmu", buf, 8); drop_of(&t->dmmu, same(buf, "no") ? "no" : "yes", yn_items, 2);
    line_get(cfg, "disk", fv(t, F_DISK), 128);
    line_get(cfg, "swap", fv(t, F_SWAP), 128);
    line_get(cfg, "cdrom", fv(t, F_CD), 128);
    line_get(cfg, "boot", fv(t, F_ARCH), 128);
    drop_of(&t->dboot, "", boot_items, 2); t->dboot.sel = same(fv(t, F_ARCH), "-") ? 1 : 0;
    line_get(cfg, "kernel", fv(t, F_KERNEL), 128);
    line_get(cfg, "args", fv(t, F_ARGS), 128);
    line_get(cfg, "autostart", buf, 8); t->fauto = same(buf, "yes");
    i = 0; while (i < N_F) { t->fcaret[i] = ui_len(fv(t, i)); ++i; }
}
static int is_path_field(int f) { return f == F_DISK || f == F_SWAP || f == F_CD || f == F_KERNEL || f == F_ARCH; }
static int is_drop_field(int f) { return f == F_CPUS || f == F_MMU || f == F_BOOT; }
static struct ui_drop *drop_for(struct machines *t, int f) { return f == F_CPUS ? &t->dcpu : f == F_MMU ? &t->dmmu : &t->dboot; }
// does field F hold what it may: the memory a number from 16 to 1024; a
// file named, one there (pc and none for the CD)
static int field_ok(struct machines *t, int f) {
    int st[4];
    char *v;
    v = fv(t, f);
    if (f == F_MB) return ui_num_ok(v, 16, 1024);
    if (!is_path_field(f) || !v[0]) return f != F_KERNEL;
    if (f == F_CD && (same(v, "pc") || same(v, "none"))) return 1;
    if (f == F_ARCH && same(v, "-")) return 1;
    return ustat(v, st) >= 0 && st[0] == 2;
}
// the form's fields of tab TAB: the first and last
static void tab_fields(int tab, int *first, int *last) {
    if (tab == 2) { *first = F_MB; *last = F_CD; } else { *first = F_BOOT; *last = F_AUTO; }
}
// "Boots from": DISK, boot - and --root hd0p1 first in args; or the system's
// archive again, and no --root. Into the form's ARGS and ARCH
static void bootfrom_args(struct machines *t, int disk) {
    char out[220], *p, sys[40];
    int k, n0;
    out[0] = 0;
    if (disk) ui_cpy(out, "--root hd0p1");
    p = fv(t, F_ARGS);
    while (*p) {
        while (*p == ' ') ++p;
        if (!*p) break;
        k = 0; while (p[k] && p[k] != ' ') ++k;
        if (k == 6 && !memcmp(p, "--root", 6)) { p = p + k; while (*p == ' ') ++p; while (*p && *p != ' ') ++p; continue; }
        if (out[0]) ui_cat(out, " ");
        n0 = ui_len(out);
        if (n0 + k < 126) { memcpy(out + n0, p, k); out[n0 + k] = 0; }
        p = p + k;
    }
    ui_ncpy(fv(t, F_ARGS), out, 127);
    if (disk) ui_cpy(fv(t, F_ARCH), "-");
    else {
        cfg_read(mname(t, t->form_row));
        line_get(cfg, "system", sys, 40);
        ui_cpy(fv(t, F_ARCH), same(sys, "c4dos") ? "/vm/images/c4dos.tar" : "/vm/images/c4ix.tar");
    }
    t->fcaret[F_ARGS] = ui_len(fv(t, F_ARGS)); t->fcaret[F_ARCH] = ui_len(fv(t, F_ARCH));
}
// Apply: the form's values into vm.cfg (each field that holds what it may)
static void form_apply(struct machines *t) {
    char *nm;
    int f, bad;
    if (!guest(t, t->form_row)) return;
    nm = mname(t, t->form_row);
    bad = 0; f = 0;
    while (f < N_F) { if (!is_drop_field(f) && f != F_AUTO && !field_ok(t, f)) bad = 1; ++f; }
    if (bad) { say(t, "Not written: a field edged in red holds what it may not."); return; }
    cfg_set(nm, "mb", fv(t, F_MB));
    cfg_set(nm, "cpus", cpu_items[t->dcpu.sel]);
    cfg_set(nm, "mmu", yn_items[t->dmmu.sel]);
    cfg_set(nm, "disk", fv(t, F_DISK)[0] ? fv(t, F_DISK) : "");
    cfg_set(nm, "swap", fv(t, F_SWAP));
    cfg_set(nm, "cdrom", fv(t, F_CD)[0] && !same(fv(t, F_CD), "none") ? fv(t, F_CD) : "");
    cfg_set(nm, "kernel", fv(t, F_KERNEL));
    cfg_set(nm, "boot", fv(t, F_ARCH)[0] ? fv(t, F_ARCH) : "-");
    cfg_set(nm, "args", fv(t, F_ARGS));
    if (!cfg_set(nm, "autostart", t->fauto ? "yes" : "no")) { say(t, "vm.cfg was not written."); return; }
    ui_cpy(t->status, nm); ui_cat(t->status, up(t, t->form_row) ? ": written, for its next start" : ": written");
    form_load(t, t->form_row);
}

// ---- Create VM ----

static void create_open(struct machines *t) {
    int i;
    t->create = 1; t->cfocus = CF_NAME; t->cauto = 0; t->cmb_said = 0;
    i = 0; while (i < N_CF) { cv(t, i)[0] = 0; t->ccaret[i] = 0; ++i; }
    drop_of(&t->csys, "", sys_items, 5); t->csys.sel = 0;
    drop_of(&t->ccpus, "1", cpu_items, 4);
    ui_cpy(cv(t, CF_MB), "256"); ui_cpy(cv(t, CF_DISK), "0"); ui_cpy(cv(t, CF_SWAP), "0");
    i = 0; while (i < N_CF) { t->ccaret[i] = ui_len(cv(t, i)); ++i; }
}
static int good_name(char *s) {
    int k;
    k = 0;
    while (s[k]) {
        if (!((s[k] >= 'a' && s[k] <= 'z') || (s[k] >= 'A' && s[k] <= 'Z') || (s[k] >= '0' && s[k] <= '9') || s[k] == '-' || s[k] == '_')) return 0;
        ++k;
    }
    return k > 0 && k < 24 && !same(s, "images") && !same(s, "control");
}
static int cfield_ok(struct machines *t, int f) {
    int st[4];
    if (f == CF_NAME) {
        // letters and digits, and no machine of that name already
        if (!good_name(cv(t, f))) return 0;
        upathjoin(path, 128, "/vm", cv(t, f));
        return ureaddir(path, 0, buf) < 0;
    }
    if (f == CF_ISO) return t->csys.sel != 4 || (ustat(cv(t, f), st) >= 0 && st[0] == 2);
    if (f == CF_MB) return ui_num_ok(cv(t, f), 16, 1024);
    if (f == CF_DISK) return ui_num_ok(cv(t, f), 0, 2047);
    if (f == CF_SWAP) return ui_num_ok(cv(t, f), 0, 1024);
    return 1;
}
static void create_go(struct machines *t) {
    char cmd[300];
    int f;
    f = 0;
    while (f < N_CF) { if (!cfield_ok(t, f)) { ui_cpy(t->status, "Not made: "); ui_cat(t->status, c_label[f]); ui_cat(t->status, " is not right (edged in red)."); return; } ++f; }
    // in a Terminal: the image being written shows there, and any error
    ui_cpy(cmd, "vm create "); ui_cat(cmd, cv(t, CF_NAME)); ui_cat(cmd, " "); ui_cat(cmd, cv(t, CF_MB));
    ui_cat(cmd, " "); ui_cat(cmd, cv(t, CF_DISK)); ui_cat(cmd, " ");
    ui_cat(cmd, t->csys.sel == 4 ? cv(t, CF_ISO) : sys_names[t->csys.sel]);
    if (t->ccpus.sel) { ui_cat(cmd, " cpus="); ui_cat(cmd, cpu_items[t->ccpus.sel]); }
    if (uatoi(cv(t, CF_SWAP)) > 0) { ui_cat(cmd, " swap="); ui_cat(cmd, cv(t, CF_SWAP)); }
    if (t->cauto) ui_cat(cmd, " autostart");
    run_command(cmd);
    t->create = 0;
    ui_cpy(t->status, cv(t, CF_NAME)); ui_cat(t->status, ": being made in a Terminal; then Start.");
}

// Shut Down or Restart (CMD) from the Go menu while machines run: they
// shut down first, in Machines' window, each as its power button asks,
// with how it goes; then CMD. 1 if it was taken so, 0 if none run
int machines_before_power(char *cmd) {
    int n, id, st, i;
    struct win *w;
    struct machines *t;
    if (!(__c4_info() & C4I_HV_BIT)) return 0;
    if (__c4_info() & 0x80000) uioperm(UIOPERM_DEV);
    n = 0; id = 1;
    while (id < *(int *)R_COUNT) {
        st = hv(C_STATE, id, 0, 0, 0);
        if (st == S_RUN || st == S_PAUSE) ++n;
        ++id;
    }
    if (!n) return 0;
    if ((i = machines_open()) < 0) return 0;
    w = win_get(i);
    t = S(w);
    ui_ncpy(t->power_cmd, cmd, 15);
    t->power = now_ms();
    if (!t->power) t->power = 1;
    id = 1;
    while (id < *(int *)R_COUNT) {
        st = hv(C_STATE, id, 0, 0, 0);
        if (st == S_PAUSE) { hv(C_RESUME, id, 0, 0, 0); st = S_RUN; }
        if (st == S_RUN) hv(C_STOP, id, 0, 0, 0);
        ++id;
    }
    refresh(t);
    mark_win_dirty(w);
    return 1;
}
// how many of the list run (or are paused)
static int n_running(struct machines *t) {
    int k, n;
    n = 0; k = CTL + 1;
    while (k < t->n) { if (up(t, k)) ++n; ++k; }
    return n;
}

int machines_open() {
    int i;
    struct win *w;
    w = win_first();
    while (w) { if (w->used && w->kind == K_VMS) { win_raise(w->id); return w->id; } w = w->next; }
    i = win_open(K_VMS, "Machines", 800, 560, sizeof(struct machines));
    if (i < 0) return i;
    w = win_get(i);
    S(w)->ltop = 0 - 1; S(w)->form_row = 0 - 1; S(w)->focus = 0 - 1; S(w)->shown = 0 - 1;
    if (!(__c4_info() & C4I_HV_BIT)) say(S(w), "No hypervisor here: GRUB's \"C4PC hypervisor\" entries boot one.");
    else {
        // the hypervisor's window is a device's: the desktop may read it
        // only once it has asked (under the MMU)
        if (__c4_info() & 0x80000) uioperm(UIOPERM_DEV);
        refresh(S(w)); say(S(w), "Ready.");
    }
    S(w)->sel = S(w)->n > CTL + 1 ? CTL + 1 : 0;
    mark_dirty();
    return i;
}

// the Log's source for row K: 0 c4pc's, 1 the control's, id + 1 a guest's
static int log_which(struct machines *t, int k) { return k == 0 ? 0 : k == CTL ? 1 : t->id[k] ? t->id[k] + 1 : 0 - 1; }
static void log_fetch(struct machines *t) {
    int w;
    logbuf[0] = 0;
    if ((w = log_which(t, t->sel)) < 0 || hv(C_LOG, (int)logbuf, LOGW + 1, w, 0) < 0) logbuf[0] = 0;
}
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
    logbuf[0] = 0;
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
    // shutting the machines down before C4IX: when none runs, C4IX's turn
    if (t->power) {
        refresh(t);
        if (!n_running(t)) {
            t->power = 0;
            say(t, "Every machine has shut down.");
            run_command(t->power_cmd);
        }
        mark_win_dirty(w);
        return;
    }
    if (t->pid && (r = uwaitpid(t->pid, st, UWNOHANG)) != 0) {
        ui_cpy(t->status, t->job);
        t->pid = 0;
        if (r > 0 && !st[0] && !st[1]) ui_cat(t->status, ": started");
        else {
            why(t->job);
            ui_cat(t->status, ": did not start: ");
            ui_ncpy(t->status + ui_len(t->status), last_line(start_log), 118 - ui_len(t->status));
            ui_cpy(why_title, t->job); ui_cat(why_title, " did not start");
            msgbox(w, why_title, start_log[0] ? start_log : "vm up said nothing (vm up NAME in a Terminal)", 0, A_NONE);
        }
    }
    refresh(t);
    if (t->tab == 5) log_fetch(t);
    if (t->restart) {
        k = CTL + 1;
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
    if (k == 0) ui_cpy(out, "the PC");
    else if (k == CTL) ui_cpy(out, "running (this C4IX)");
    else if (s == S_RUN) ui_cpy(out, "running");
    else if (s == S_PAUSE) ui_cpy(out, "paused");
    else if (s == S_HALT) ui_cpy(out, "stopped");
    else if (s == S_MADE) ui_cpy(out, "made, not started");
    else ui_cpy(out, "stopped");
    if (guest(t, k) && t->pid && same(t->job, mname(t, k))) ui_cpy(out, "starting...");
}

// a monitor: its screen green when it runs, amber paused
static void icon(int x, int y, int st) {
    d_rect(x, y + 1, 16, 11, C_DARK);
    d_rect(x + 2, y + 3, 12, 7, st == S_RUN ? 0x30c030 : st == S_PAUSE ? 0xe0a000 : 0x707070);
    d_rect(x + 6, y + 12, 4, 2, C_SHADOW);
    d_rect(x + 3, y + 14, 10, 1, C_SHADOW);
}

static void field(int x, int y, char *label, char *value) {
    d_text(x, y, C_DARK, F_SANS, label);
    d_text(x + 104, y, C_DARK, F_BOLD, value);
}

static void graph(int *h, int n, int x, int y, int w, int ht, int max) {
    int k, px, py, qx, qy;
    d_rect(x, y, w, ht, C_DARK);
    k = 1; while (k < 4) { d_rect(x, y + (ht * k) / 4, w, 1, 0x005000); ++k; }
    if (!max) { max = 1; k = 0; while (k < n) { if (h[k] > max) max = h[k]; ++k; } }
    if (n < 2) return;
    k = 1;
    while (k < n) {
        px = x + w - 1 - (n - k) * (w - 2) / (HIST - 1);
        qx = x + w - 1 - (n - 1 - k) * (w - 2) / (HIST - 1);
        py = y + ht - 2 - (int)(((long long)(h[k - 1] > max ? max : h[k - 1]) * (ht - 4)) / max);
        qy = y + ht - 2 - (int)(((long long)(h[k] > max ? max : h[k]) * (ht - 4)) / max);
        d_line(px, py, qx, qy, C_GREEN);
        ++k;
    }
}

// the screen box: as large as fits W x H at 4:3 (its size kept for grab)
static void screen_box(struct machines *t, int x, int y, int w, int h) {
    int s, k, sw, sh;
    sw = w; if (sw > SW_MAX) sw = SW_MAX;
    sh = sw * 3 / 4;
    if (sh > h) { sh = h; sw = sh * 4 / 3; }
    sh = sh / STRIP * STRIP; if (sh > SH_MAX) sh = SH_MAX;
    sw = sh * 4 / 3;
    if (sw != t->sw || sh != t->sh) { t->sw = sw; t->sh = sh; t->shown = 0 - 1; }
    x = x + (w - sw) / 2;
    d_rect(x, y, sw, sh, C_DARK);
    if (t->shown == t->sel && t->sel >= CTL && (t->sel == CTL || up(t, t->sel))) {
        s = 0; k = 0;
        while (s < sh) { d_image(IMG_ID + k, x, y + s); s = s + STRIP; ++k; }
    } else d_text(x + sw / 2 - 33, y + sh / 2 - 6, 0xa0a0a0, F_SANS, t->sel >= CTL && (t->sel == CTL || up(t, t->sel)) ? "..." : "Not running");
}

static void rate_text(struct machines *t, int k, char *out) {
    if (k > CTL && t->state[k] != S_RUN) ui_cpy(out, "-");
    else if (t->rate[k] >= 1000) { ui_num_commas(out, t->rate[k] / 1000); ui_cat(out, " M instructions/s"); }
    else { ui_num(out, t->rate[k]); ui_cat(out, " K instructions/s (idle)"); }
}

static void draw_host_summary(struct machines *t, int x, int y, int w, int h) {
    int k, g, mbs;
    char *p;
    ui_group(x, y, w, 196, "c4pc, the PC");
    line_get(host_buf, "processor", buf, 80); field(x + 12, y + 22, "Processor", buf[0] ? buf : "-");
    line_get(host_buf, "processors", buf, 80); field(x + 12, y + 42, "Processors", buf[0] ? buf : "1");
    line_get(host_buf, "memory", buf, 40); field(x + 12, y + 62, "Memory", buf[0] ? buf : "-");
    // the disks' MB, added
    mbs = 0; p = host_buf;
    while (*p) { if (!memcmp(p, "disk ", 5)) mbs = mbs + (int)num_of(p + 5); while (*p && *p != 10) ++p; if (*p) ++p; }
    ui_num_commas(buf, mbs); ui_cat(buf, " MB on the disks"); field(x + 12, y + 82, "Storage", buf);
    ui_num(buf, t->n - CTL - 1); field(x + 12, y + 102, "Machines", buf);
    k = CTL + 1; g = 0; while (k < t->n) { if (t->state[k] == S_RUN) ++g; ++k; }
    ui_num(buf, g); field(x + 12, y + 122, "Running", buf);
    d_text(x + 12, y + 150, C_DARK, F_SANS, "Create VM makes a machine; the control C4IX is this one.");
    d_text(x + 12, y + 168, C_DARK, F_SANS, "Hardware: what c4pc found. Load: its time busy. Log: what it said.");
    if (h > 290) {
        ui_group(x, y + 206, w, h - 206, "CPU 0: its time busy, %, the last minute");
        graph(t->chist, t->nchist[0], x + 8, y + 224, w - 16, h - 232, 100);
    }
}

static void draw_summary(struct machines *t, int x, int y, int w, int h) {
    int k, cx, sy, gw;
    k = t->sel;
    if (k == 0) { draw_host_summary(t, x, y, w, h); return; }
    // the status, compact, two columns; the CPU beside it; the screen under
    gw = w / 3;
    ui_group(x, y, w - gw - 8, 112, "Status");
    state_text(t, k, buf); field(x + 10, y + 20, "State", buf);
    buf[0] = 0; if (k == CTL) ui_cpy(buf, "0"); else if (t->id[k]) ui_num(buf, t->id[k]); else ui_cpy(buf, "-");
    field(x + 10, y + 40, "ID", buf);
    ui_num(buf, t->mb[k]); ui_cat(buf, " MB"); field(x + 10, y + 60, "Memory", buf);
    ui_num(buf, t->ncpu[k]); field(x + 10, y + 80, "CPUs", buf);
    cx = x + (w - gw - 8) / 2;
    if (k == CTL) {
        field(cx, y + 20, "System", "C4IX (the control)");
        field(cx, y + 40, "Its disks", "the PC's");
        field(cx, y + 60, "Settings", "its GRUB entry's");
    } else if (t->cfg[k] && cfg_read(mname(t, k)) >= 0) {
        line_get(cfg, "system", buf, 30); field(cx, y + 20, "System", buf[0] ? buf : "-");
        line_get(cfg, "disk", buf, 60); field(cx, y + 40, "Hard disk", buf[0] ? ui_base(buf) : "none");
        line_get(cfg, "cdrom", buf, 60); field(cx, y + 60, "CD/DVD", !buf[0] ? "none" : same(buf, "pc") ? "the PC's drive" : ui_base(buf));
        line_get(cfg, "autostart", buf, 8); field(cx, y + 80, "Start at boot", same(buf, "yes") ? "yes" : "no");
    } else d_text(cx, y + 20, C_DARK, F_SANS, "made by vm new: no vm.cfg");
    ui_group(x + w - gw, y, gw, 112, "CPU, a second");
    rate_text(t, k, buf); d_text(x + w - gw + 8, y + 18, C_DARK, F_SANS, buf);
    graph(t->hist + k * HIST, t->nhist[k], x + w - gw + 8, y + 34, gw - 16, 70, 0);
    sy = y + 120;
    ui_group(x, sy, w, h - (sy - y), "Screen");
    screen_box(t, x + 8, sy + 18, w - 16, h - (sy - y) - 26);
}

static void draw_console(struct machines *t, int x, int y, int w, int h) {
    if (t->sel == 0) { d_text(x + 8, y + 8, C_DARK, F_SANS, "c4pc has no screen of its own here: choose a machine."); return; }
    screen_box(t, x, y + 4, w, h - 70);
    if (t->sel == CTL) { d_text(x + (w - 300) / 2, y + h - 52, C_DARK, F_SANS, "The control C4IX's screen: the one you are at."); return; }
    ui_button(x + (w - 220) / 2, y + h - 58, 220, 26, "Attach: its screen, keys, mouse", 0, 1);
    d_text(x + (w - 280) / 2, y + h - 24, C_DARK, F_SANS, "Ctrl+Alt+Home brings this desktop back.");
}

// what c4pc found of the PC: its HOST lines, a row each
static void draw_host_hw(struct machines *t, int x, int y, int w) {
    char *p, key[24];
    int r, k;
    d_rect(x, y, w, 330, C_WHITE); d_bevel(x, y, w, 330, B_SUNKEN);
    d_text(x + 8, y + 4, C_DARK, F_BOLD, "Hardware");
    d_text(x + 150, y + 4, C_DARK, F_BOLD, "What c4pc found");
    p = host_buf; r = 0;
    while (*p && r < 14) {
        k = 0; while (*p && *p != ' ' && *p != 10 && k < 23) { key[k] = *p; ++k; ++p; }
        key[k] = 0;
        while (*p == ' ') ++p;
        k = 0; while (*p && *p != 10 && k < 120) { buf[k] = *p; ++k; ++p; }
        buf[k] = 0;
        if (*p) ++p;
        if (same(key, "uptime") || same(key, "idle")) continue;
        d_text(x + 8, y + 24 + r * ROW_H, C_DARK, F_SANS, key);
        d_text(x + 150, y + 24 + r * ROW_H, C_DARK, F_SANS, buf);
        ++r;
    }
    if (!r) d_text(x + 8, y + 24, C_DARK, F_SANS, "(this c4pc says nothing of its hardware: an older one)");
}
static void draw_ctl_hw(struct machines *t, int x, int y, int w) {
    int n;
    d_rect(x, y, w, 150, C_WHITE); d_bevel(x, y, w, 150, B_SUNKEN);
    ui_num(buf, t->mb[CTL]); ui_cat(buf, " MB"); field(x + 8, y + 8, "Memory", buf);
    ui_num(buf, t->ncpu[CTL]); field(x + 8, y + 30, "CPUs", buf);
    field(x + 8, y + 52, "MMU", (__c4_info() & 0x80000) ? "yes" : "no");
    n = uslurp("/proc/disks", buf, 190); if (n < 0) n = 0; buf[n] = 0;
    { char *p; p = buf; while (*p) { if (*p == 10) *p = ' '; ++p; } }
    field(x + 8, y + 74, "Disks", buf[0] ? buf : "none");
    d_text(x + 8, y + 110, C_DARK, F_SANS, "The control C4IX's settings are its GRUB entry's: shown, not changed here.");
}

// the form's rows of tab TAB, at X, Y
static void draw_form(struct machines *t, int x, int y, int w, int focused) {
    int f, first, last, r, fy, fw;
    tab_fields(t->tab, &first, &last);
    f = first; r = 0;
    while (f <= last) {
        fy = y + 8 + r * FROW;
        d_text(x + 8, fy + 5, C_DARK, F_SANS, f_label[f]);
        fw = w - FLX - 16;
        if (is_path_field(f)) fw = fw - 90;
        if (is_drop_field(f)) ui_drop(x + FLX, fy, 220, drop_for(t, f));
        else if (f == F_AUTO) ui_check(x + FLX, fy + 4, "start it when the hypervisor starts", t->fauto);
        else {
            ui_field(x + FLX, fy, fw, fv(t, f), t->fcaret[f], focused && t->focus == f);
            if (!field_ok(t, f)) ui_field_bad(x + FLX, fy, fw);
        }
        if (is_path_field(f)) ui_button(x + FLX + fw + 8, fy, 82, 22, "Browse...", 0, 0);
        ++f; ++r;
    }
    fy = y + 8 + r * FROW + 8;
    ui_button(x + FLX, fy, 90, 26, "Apply", 0, 1);
    ui_button(x + FLX + 100, fy, 90, 26, "Revert", 0, 0);
    d_text(x + 8, fy + 36, C_DARK, F_SANS, t->tab == 2 ? "The machine has these at its next start. CD: an ISO file, pc (the PC's drive) or none."
                                                     : "Boots from its disk: once install, in it, has put C4IX on its hd0.");
}
static void draw_form_lists(struct machines *t, int x, int y) {
    int f, first, last, r;
    tab_fields(t->tab, &first, &last);
    f = first; r = 0;
    while (f <= last) { if (is_drop_field(f)) ui_drop_list(x + FLX, y + 8 + r * FROW, 220, drop_for(t, f)); ++f; ++r; }
}
static void draw_cfgtab(struct machines *t, int x, int y, int w, int focused) {
    if (t->sel == 0) {
        if (t->tab == 2) draw_host_hw(t, x, y, w);
        else d_text(x + 8, y + 8, C_DARK, F_SANS, "c4pc's options are its GRUB entry's (the hypervisor's: --hv).");
        return;
    }
    if (t->sel == CTL) { draw_ctl_hw(t, x, y, w); return; }
    if (!t->cfg[t->sel]) { d_text(x + 8, y + 8, C_DARK, F_SANS, "This machine has no vm.cfg (vm new made it)."); return; }
    if (t->form_row != t->sel) form_load(t, t->sel);
    draw_form(t, x, y, w, focused);
}

// Load: each CPU's instructions a second (c4pc: CPU 0's time busy, %)
static void draw_load(struct machines *t, int x, int y, int w, int h) {
    int k, c, n, cols, rows, gw, gh, gx, gy;
    k = t->sel;
    n = k == 0 ? 1 : t->ncpu[k];
    if (n < 1) n = 1;
    if (k > CTL && t->state[k] != S_RUN) { d_text(x + 8, y + 8, C_DARK, F_SANS, "It is not running."); return; }
    cols = n > 1 ? 2 : 1; rows = (n + cols - 1) / cols;
    gw = (w - (cols - 1) * 10) / cols; gh = (h - 8) / rows - 20;
    if (gh > 200) gh = 200;
    c = 0;
    while (c < n) {
        gx = x + (c % cols) * (gw + 10); gy = y + (c / cols) * (gh + 20);
        ui_cpy(buf, "CPU "); ui_num(buf + 4, c);
        ui_cat(buf, k == 0 ? ": its time busy, %" : ": instructions a second");
        d_text(gx, gy, C_DARK, F_SANS, buf);
        graph(t->chist + (k * MAXC + c) * HIST, t->nchist[k], gx, gy + 16, gw, gh, k == 0 ? 100 : 0);
        ++c;
    }
    if (k == 0) {
        line_get(host_buf, "processors", buf, 80);
        d_text(x, y + h - 14, C_DARK, F_SANS, "c4pc's CPU 0 runs the control C4IX and the machines in turn");
    }
}

// Log: its source's lines (c4pc's, the control's, a guest's), scrolled
static void draw_log(struct machines *t, int x, int y, int w, int h) {
    int n, k, rows, cols, top, c;
    char *p, line[160];
    d_rect(x, y, w - 16, h, C_DARK);
    if (!logbuf[0]) log_fetch(t);
    n = 0; p = logbuf;
    while (*p) {
        if (n == LOG_MAX) { k = 0; while (k < LOG_MAX - 1) { log_at[k] = log_at[k + 1]; ++k; } --n; }
        log_at[n] = p - logbuf; ++n;
        while (*p && *p != 10) ++p;
        if (*p) ++p;
    }
    rows = (h - 8) / LROW; cols = (w - 24) / 7;
    if (cols > 159) cols = 159;
    t->lrows = rows; t->ln = n;
    top = t->ltop < 0 || t->ltop > n - rows ? (n > rows ? n - rows : 0) : t->ltop;
    if (!n) d_text(x + 6, y + 6, 0xa0a0a0, F_SANS, guest(t, t->sel) && !t->id[t->sel] ? "Not made yet: its log is its console's, once it runs." : "Nothing yet.");
    k = top;
    while (k < n && k < top + rows) {
        p = logbuf + log_at[k]; c = 0;
        while (p[c] && p[c] != 10 && c < cols) { line[c] = p[c]; ++c; }
        line[c] = 0;
        d_text(x + 4, y + 4 + (k - top) * LROW, 0xd0d0d0, F_MONO, line);
        ++k;
    }
    ui_scrollbar(x + w - 16, y, h, n, rows, top);
}

// Create VM's form, over the page
static void draw_create(struct machines *t, int x, int y, int w, int focused) {
    int f, fy, fw, r;
    d_text(x + 8, y + 4, C_DARK, F_BOLD, "Create VM");
    f = 0; r = 0;
    while (f < N_CF) {
        if (f == CF_ISO && t->csys.sel != 4) { ++f; continue; }
        fy = y + 28 + r * FROW;
        d_text(x + 8, fy + 5, C_DARK, F_SANS, c_label[f]);
        fw = w - 230 - 16 - (f == CF_ISO ? 90 : 0);
        if (f == CF_SYS) ui_drop(x + 230, fy, 280, &t->csys);
        else if (f == CF_CPUS) ui_drop(x + 230, fy, 120, &t->ccpus);
        else if (f == CF_AUTO) ui_check(x + 230, fy + 4, "start it when the hypervisor starts", t->cauto);
        else {
            if (f != CF_NAME && f != CF_ISO) fw = 120;
            ui_field(x + 230, fy, fw, cv(t, f), t->ccaret[f], focused && t->cfocus == f);
            if (!cfield_ok(t, f) && (f != CF_NAME || cv(t, f)[0])) ui_field_bad(x + 230, fy, fw);
        }
        if (f == CF_ISO) ui_button(x + 230 + fw + 8, fy, 82, 22, "Browse...", 0, 0);
        ++f; ++r;
    }
    fy = y + 28 + r * FROW + 10;
    ui_button(x + 230, fy, 100, 26, "Create", 0, 1);
    ui_button(x + 340, fy, 100, 26, "Cancel", 0, 0);
    d_text(x + 8, fy + 40, C_DARK, F_SANS, "A name of letters, digits, - and _. Memory 16 to 1024 MB; a disk image 0 to 2047 MB");
    d_text(x + 8, fy + 58, C_DARK, F_SANS, "(512 or more to install C4IX on it); swap 0 to 1024 MB (C4IX's MMU puts pages there).");
}
static void create_lists(struct machines *t, int x, int y) {
    int r;
    r = t->csys.sel == 4 ? 4 : 3;
    ui_drop_list(x + 230, y + 28 + r * FROW, 120, &t->ccpus);
    ui_drop_list(x + 230, y + 28 + 1 * FROW, 280, &t->csys);
}
// the shutting down's page: each machine and how it is, and the buttons
static void draw_power(struct machines *t, int x, int y, int w, int h) {
    int k, r, secs;
    d_text(x + 8, y + 8, C_DARK, F_BOLD, same(t->power_cmd, "reboot") ? "Restarting: the machines shut down first" : "Shutting down: the machines shut down first");
    d_text(x + 8, y + 30, C_DARK, F_SANS, "Each is asked as its power button asks (SIGTERM), so that it unmounts its disks cleanly.");
    r = 0; k = CTL + 1;
    while (k < t->n) {
        if (t->id[k]) {
            icon(x + 12, y + 60 + r * ROW_H, t->state[k]);
            d_text(x + 36, y + 62 + r * ROW_H, C_DARK, F_SANS, mname(t, k));
            d_text(x + 220, y + 62 + r * ROW_H, C_DARK, F_SANS, t->state[k] == S_RUN ? "shutting down..." : "stopped");
            ++r;
        }
        ++k;
    }
    secs = (now_ms() - t->power) / 1000;
    ui_num(buf, secs); ui_cat(buf, " s; "); ui_num(buf + ui_len(buf), n_running(t)); ui_cat(buf, " still running");
    d_text(x + 8, y + 72 + r * ROW_H, C_DARK, F_SANS, buf);
    ui_button(x + 8, y + 100 + r * ROW_H, 160, 26, "Stop them now", 0, secs >= 30);
    ui_button(x + 180, y + 100 + r * ROW_H, 100, 26, "Cancel", 0, 0);
}
static int power_rows(struct machines *t) {
    int k, n;
    n = 0; k = CTL + 1; while (k < t->n) { if (t->id[k]) ++n; ++k; }
    return n;
}

void machines_draw(struct win *w, int ox, int oy, int cw, int ch, int focused) {
    struct machines *t;
    int k, y, rx, rw, bx;
    t = S(w);
    tabs[0] = "Summary"; tabs[1] = "Console"; tabs[2] = "Hardware"; tabs[3] = "Options"; tabs[4] = "Load"; tabs[5] = "Log";
    d_rect(ox, oy, cw, ch, C_FACE);
    // the toolbar
    ui_button(ox + 6, oy + 5, 100, 24, "Create VM", 0, 0);
    bx = ox + cw - 6 - N_TB * TB;
    ui_button(bx, oy + 5, TBW, 24, "Start", 0, 0);
    ui_button(bx + TB, oy + 5, TBW, 24, "Shutdown", 0, 0);
    ui_button(bx + 2 * TB, oy + 5, TBW, 24, "Stop", 0, 0);
    ui_button(bx + 3 * TB, oy + 5, TBW, 24, "Restart", 0, 0);
    ui_button(bx + 4 * TB, oy + 5, TBW, 24, guest(t, t->sel) && t->state[t->sel] == S_PAUSE ? "Resume" : "Pause", 0, 0);
    ui_button(bx + 5 * TB, oy + 5, TBW, 24, "Attach", 0, 0);
    ui_button(bx + 6 * TB, oy + 5, TBW, 24, "Delete", 0, 0);
    // the tree: c4pc, and under it the control C4IX and the machines
    d_rect(ox + 4, oy + TOOL_H, TREE_W, ch - TOOL_H - 26, C_WHITE);
    d_bevel(ox + 4, oy + TOOL_H, TREE_W, ch - TOOL_H - 26, B_SUNKEN);
    y = oy + TOOL_H + 4;
    k = 0;
    while (k < t->n) {
        if (k == t->sel) d_rect(ox + 6, y - 1, TREE_W - 4, ROW_H - 2, C_SEL);
        if (k == 0) { d_icon(K_VMS, ox + 10, y + 1); d_text(ox + 30, y + 3, k == t->sel ? C_WHITE : C_DARK, F_BOLD, "c4pc"); }
        else {
            d_rect(ox + 14, y + 2, 1, ROW_H - 6, C_SHADOW);
            icon(ox + 22, y + 2, k == CTL ? S_RUN : t->state[k]);
            buf[0] = 0;
            if (k == CTL) ui_cpy(buf, "0 control (this C4IX)");
            else { if (t->id[k]) { ui_num(buf, t->id[k]); ui_cat(buf, " "); } ui_cat(buf, mname(t, k)); }
            d_text(ox + 44, y + 3, k == t->sel ? C_WHITE : C_DARK, k == CTL ? F_BOLD : F_SANS, buf);
        }
        y = y + ROW_H;
        ++k;
    }
    // the page
    rx = ox + TREE_W + 10; rw = cw - TREE_W - 14;
    if (t->power || t->create) {
        d_rect(ox + 2, oy + TOOL_H, cw - 4, ch - TOOL_H - 26, C_FACE);
        if (t->power) draw_power(t, ox + 8, oy + TOOL_H + 4, cw - 16, ch - TOOL_H - 34);
        else { draw_create(t, ox + 8, oy + TOOL_H + 4, cw - 16, focused); create_lists(t, ox + 8, oy + TOOL_H + 4); }
        d_bevel(ox + 2, oy + ch - 22, cw - 4, 20, B_SUNKEN);
        d_text(ox + 8, oy + ch - 18, C_DARK, F_SANS, t->status);
        return;
    }
    d_bevel(rx, oy + PAGE_Y, rw, ch - PAGE_Y - 26, B_RAISED);
    ui_tabs(rx + 4, oy + TAB_Y, tabs, N_TABS, t->tab);
    if (t->tab == 0) draw_summary(t, rx + 8, oy + PAGE_Y + 10, rw - 16, ch - PAGE_Y - 44);
    else if (t->tab == 1) draw_console(t, rx + 8, oy + PAGE_Y + 8, rw - 16, ch - PAGE_Y - 40);
    else if (t->tab == 2 || t->tab == 3) draw_cfgtab(t, rx + 8, oy + PAGE_Y + 10, rw - 16, focused);
    else if (t->tab == 4) draw_load(t, rx + 8, oy + PAGE_Y + 10, rw - 16, ch - PAGE_Y - 44);
    else draw_log(t, rx + 8, oy + PAGE_Y + 8, rw - 16, ch - PAGE_Y - 40);
    d_bevel(ox + 2, oy + ch - 22, cw - 4, 20, B_SUNKEN);
    d_text(ox + 8, oy + ch - 18, C_DARK, F_SANS, t->status);
    // an open dropdown's list, over the rest
    if ((t->tab == 2 || t->tab == 3) && guest(t, t->sel) && t->form_row == t->sel) draw_form_lists(t, rx + 8, oy + PAGE_Y + 10);
}

// ---- input ----

// the form's mouse: its row's field, drop, Browse, check; Apply, Revert.
// 1 if it took the event
static int form_mouse(struct win *w, struct machines *t, int ev, int x, int y, int fx, int fy, int fwid) {
    int f, first, last, r, ry, fw, k;
    tab_fields(t->tab, &first, &last);
    // an open list first: it is over the rest
    f = first; r = 0;
    while (f <= last) {
        if (is_drop_field(f) && drop_for(t, f)->open) {
            k = ui_drop_mouse(ev, x, y, fx + FLX, fy + 8 + r * FROW, 220, drop_for(t, f));
            if (k == 2 && f == F_BOOT) bootfrom_args(t, t->dboot.sel);
            if (k) return 1;
        }
        ++f; ++r;
    }
    if (ev != E_DOWN && ev != E_DBL) return 0;
    f = first; r = 0;
    while (f <= last) {
        ry = fy + 8 + r * FROW;
        fw = fwid - FLX - 16;
        if (is_path_field(f)) fw = fw - 90;
        if (is_drop_field(f)) { if (ui_drop_mouse(ev, x, y, fx + FLX, ry, 220, drop_for(t, f))) { t->focus = 0 - 1; return 1; } }
        else if (f == F_AUTO) { if (ui_hit(x, y, fx + FLX, ry, 300, 22)) { t->fauto = !t->fauto; return 1; } }
        else if (ui_hit(x, y, fx + FLX, ry, fw, 22)) {
            t->focus = f; t->fcaret[f] = ui_field_hit(x, fx + FLX, fw, fv(t, f), t->fcaret[f]);
            return 1;
        }
        if (is_path_field(f) && ui_hit(x, y, fx + FLX + fw + 8, ry, 82, 22)) {
            ui_ncpy(browse, fv(t, f)[0] && !same(fv(t, f), "pc") && !same(fv(t, f), "-") ? fv(t, f) : "/vm/", WK_PATH - 1);
            wk_filebox(w->id, A_BROWSE + f, 0, "*", browse, WK_PATH);
            return 1;
        }
        ++f; ++r;
    }
    ry = fy + 8 + r * FROW + 8;
    if (ui_hit(x, y, fx + FLX, ry, 90, 26)) { form_apply(t); return 1; }
    if (ui_hit(x, y, fx + FLX + 100, ry, 90, 26)) { form_load(t, t->sel); say(t, "As vm.cfg has it."); return 1; }
    return 0;
}
// Create VM's mouse
static void create_mouse(struct win *w, struct machines *t, int ev, int x, int y, int cx, int cy, int cwid) {
    int f, r, fy, fw, k, isor;
    isor = t->csys.sel == 4;
    // an open list first
    if (t->csys.open) {
        k = ui_drop_mouse(ev, x, y, cx + 230, cy + 28 + FROW, 280, &t->csys);
        if (k == 2 && !t->cmb_said) { ui_num(cv(t, CF_MB), sys_mb[t->csys.sel]); t->ccaret[CF_MB] = ui_len(cv(t, CF_MB)); }
        if (k) return;
    }
    if (t->ccpus.open) { if (ui_drop_mouse(ev, x, y, cx + 230, cy + 28 + (isor ? 4 : 3) * FROW, 120, &t->ccpus)) return; }
    if (ev != E_DOWN && ev != E_DBL) return;
    f = 0; r = 0;
    while (f < N_CF) {
        if (f == CF_ISO && !isor) { ++f; continue; }
        fy = cy + 28 + r * FROW;
        fw = cwid - 230 - 16 - (f == CF_ISO ? 90 : 0);
        if (f != CF_NAME && f != CF_ISO) fw = 120;
        if (f == CF_SYS) { if (ui_drop_mouse(ev, x, y, cx + 230, fy, 280, &t->csys)) return; }
        else if (f == CF_CPUS) { if (ui_drop_mouse(ev, x, y, cx + 230, fy, 120, &t->ccpus)) return; }
        else if (f == CF_AUTO) { if (ui_hit(x, y, cx + 230, fy, 300, 22)) { t->cauto = !t->cauto; return; } }
        else if (ui_hit(x, y, cx + 230, fy, fw, 22)) { t->cfocus = f; t->ccaret[f] = ui_field_hit(x, cx + 230, fw, cv(t, f), t->ccaret[f]); return; }
        if (f == CF_ISO && ui_hit(x, y, cx + 230 + fw + 8, fy, 82, 22)) {
            ui_ncpy(browse, cv(t, f)[0] ? cv(t, f) : "/", WK_PATH - 1);
            wk_filebox(w->id, A_CBROWSE + f, 0, "*.iso", browse, WK_PATH);
            return;
        }
        ++f; ++r;
    }
    fy = cy + 28 + r * FROW + 10;
    if (ui_hit(x, y, cx + 230, fy, 100, 26)) create_go(t);
    else if (ui_hit(x, y, cx + 340, fy, 100, 26)) { t->create = 0; say(t, "Not made."); }
}

void machines_mouse(struct win *w, int ev, int x, int y, int cw, int ch) {
    struct machines *t;
    int k, rx, rw, bx, top;
    t = S(w);
    if (t->power) {
        if (ev != E_DOWN) return;
        // Stop them now, or Cancel (they go on shutting down; C4IX does not)
        k = power_rows(t);
        if (ui_hit(x, y, 16, TOOL_H + 4 + 100 + k * ROW_H, 160, 26)) {
            k = CTL + 1;
            while (k < t->n) { if (t->state[k] == S_RUN) hv(C_STOP, t->id[k], 1, 0, 0); ++k; }
            say(t, "Stopped at once.");
        } else if (ui_hit(x, y, 188, TOOL_H + 4 + 100 + k * ROW_H, 100, 26)) {
            t->power = 0;
            say(t, "Shutting down C4IX was cancelled; the machines asked to shut down go on doing so.");
        }
        mark_win_dirty(w);
        return;
    }
    if (t->create) { create_mouse(w, t, ev, x, y, 8, TOOL_H + 4, cw - 16); mark_win_dirty(w); return; }
    rx = TREE_W + 10; rw = cw - TREE_W - 14;
    // the Log's scroll bar and wheel
    if (t->tab == 5) {
        if (ev == E_WHEEL) {
            top = t->ltop < 0 ? (t->ln > t->lrows ? t->ln - t->lrows : 0) : t->ltop;
            top = top + y * 3;
            if (top >= t->ln - t->lrows) top = 0 - 1;
            else if (top < 0) top = 0;
            t->ltop = top;
            mark_win_dirty(w);
            return;
        }
        if (ev == E_MOVE && ui_scroll_active()) {
            top = ui_scroll_drag(x, y, y);
            if (top >= 0) { t->ltop = top >= t->ln - t->lrows ? 0 - 1 : top; mark_win_dirty(w); }
            return;
        }
        if (ev == E_UP) { ui_scroll_end(); return; }
        if (ev == E_DOWN) {
            top = t->ltop < 0 ? (t->ln > t->lrows ? t->ln - t->lrows : 0) : t->ltop;
            top = ui_scroll_click(x, y, rx + 8 + rw - 16 - 16, PAGE_Y + 8, ch - PAGE_Y - 40, t->ln, t->lrows, top);
            if (top >= 0) { t->ltop = top >= t->ln - t->lrows ? 0 - 1 : top; mark_win_dirty(w); return; }
        }
    }
    // the form (an open list takes moves and lets-go too)
    if ((t->tab == 2 || t->tab == 3) && guest(t, t->sel) && t->form_row == t->sel) {
        if (form_mouse(w, t, ev, x, y, rx + 8, PAGE_Y + 10, rw - 16)) { mark_win_dirty(w); return; }
    }
    if (ev != E_DOWN && ev != E_DBL) return;
    bx = cw - 6 - N_TB * TB;
    if (ui_hit(x, y, 6, 5, 100, 24)) { create_open(t); mark_win_dirty(w); return; }
    if (y >= 5 && y < 29 && x >= bx) {
        k = (x - bx) / TB;
        if (k == 0) start_one(t, t->sel);
        else if (k == 1) stop_one(t, t->sel, 0);
        else if (k == 2) stop_one(t, t->sel, 1);
        else if (k == 3 && guest(t, t->sel) && t->state[t->sel] == S_RUN) { t->restart = t->id[t->sel]; stop_one(t, t->sel, 1); }
        else if (k == 3) start_one(t, t->sel);
        else if (k == 4) pause_one(t, t->sel);
        else if (k == 5) attach_one(t, t->sel);
        else if (k == 6) delete_ask(w, t->sel);
        mark_win_dirty(w);
        return;
    }
    if (ui_hit(x, y, 4, TOOL_H, TREE_W, ch - TOOL_H - 26)) {
        k = (y - TOOL_H - 4) / ROW_H;
        if (k >= 0 && k < t->n && k != t->sel) { t->sel = k; t->shown = 0 - 1; t->ltop = 0 - 1; t->form_row = 0 - 1; logbuf[0] = 0; grab(t); }
        if (ev == E_DBL && guest(t, k) && t->state[k] == S_RUN) hv(C_ATTACH, t->id[k], 0, 0, 0);
        mark_win_dirty(w);
        return;
    }
    k = ui_tab_hit(x, y, rx + 4, TAB_Y, tabs, N_TABS);
    if (k >= 0) { t->tab = k; t->shown = 0 - 1; t->ltop = 0 - 1; t->form_row = 0 - 1; logbuf[0] = 0; grab(t); mark_win_dirty(w); return; }
    if (t->tab == 1 && guest(t, t->sel) && ui_hit(x, y, rx + 8 + (rw - 16 - 220) / 2, ch - 40 - 58 + 8, 220, 26)) {
        attach_one(t, t->sel);
        return;
    }
}

void machines_key(struct win *w, int code, int ch, int mods) {
    struct machines *t;
    int f, first, last;
    t = S(w);
    if (t->create) {
        if (code == KEY_ESC) { t->create = 0; say(t, "Not made."); }
        else if (t->csys.open) { if (ui_drop_key(&t->csys, code) == 2 && !t->cmb_said) { ui_num(cv(t, CF_MB), sys_mb[t->csys.sel]); t->ccaret[CF_MB] = ui_len(cv(t, CF_MB)); } }
        else if (t->ccpus.open) ui_drop_key(&t->ccpus, code);
        else if (code == KEY_ENTER) create_go(t);
        else if (code == KEY_TAB) {
            f = t->cfocus;
            while (1) { f = (f + 1) % N_CF; if (f == CF_NAME || f == CF_MB || f == CF_DISK || f == CF_SWAP || (f == CF_ISO && t->csys.sel == 4)) break; }
            t->cfocus = f;
        } else if (t->cfocus == CF_MB || t->cfocus == CF_DISK || t->cfocus == CF_SWAP) {
            if (ui_numfield_key(cv(t, t->cfocus), 9, t->ccaret + t->cfocus, code, ch, mods) && t->cfocus == CF_MB) t->cmb_said = 1;
        } else if (t->cfocus >= 0) ui_field_key(cv(t, t->cfocus), 127, t->ccaret + t->cfocus, code, ch, mods);
        mark_win_dirty(w);
        return;
    }
    // the form's field with the keyboard
    if ((t->tab == 2 || t->tab == 3) && guest(t, t->sel) && t->form_row == t->sel) {
        tab_fields(t->tab, &first, &last);
        f = first;
        while (f <= last) {
            if (is_drop_field(f) && drop_for(t, f)->open) {
                if (ui_drop_key(drop_for(t, f), code) == 2 && f == F_BOOT) bootfrom_args(t, t->dboot.sel);
                mark_win_dirty(w);
                return;
            }
            ++f;
        }
        if (t->focus >= first && t->focus <= last) {
            if (code == KEY_ENTER) form_apply(t);
            else if (code == KEY_ESC) t->focus = 0 - 1;
            else if (code == KEY_TAB) {
                f = t->focus;
                while (1) { ++f; if (f > last) f = first; if (!is_drop_field(f) && f != F_AUTO) break; }
                t->focus = f;
            } else if (t->focus == F_MB) ui_numfield_key(fv(t, F_MB), 5, t->fcaret + F_MB, code, ch, mods);
            else ui_field_key(fv(t, t->focus), 127, t->fcaret + t->focus, code, ch, mods);
            mark_win_dirty(w);
            return;
        }
    }
    if (code == KEY_UP && t->sel > 0) { --t->sel; t->shown = 0 - 1; t->form_row = 0 - 1; t->ltop = 0 - 1; logbuf[0] = 0; }
    else if (code == KEY_DOWN && t->sel < t->n - 1) { ++t->sel; t->shown = 0 - 1; t->form_row = 0 - 1; t->ltop = 0 - 1; logbuf[0] = 0; }
    else if (code == KEY_TAB && (mods & M_CTRL)) { t->tab = (t->tab + 1) % N_TABS; t->form_row = 0 - 1; t->ltop = 0 - 1; logbuf[0] = 0; }
    else if (t->tab == 5 && (code == KEY_PGUP || code == KEY_PGDN)) {
        f = t->ltop < 0 ? (t->ln > t->lrows ? t->ln - t->lrows : 0) : t->ltop;
        f = f + (code == KEY_PGUP ? 0 - t->lrows : t->lrows);
        if (f < 0) f = 0;
        t->ltop = f >= t->ln - t->lrows ? 0 - 1 : f;
    }
    else return;
    grab(t);
    mark_win_dirty(w);
}

void machines_answer(struct win *w, int action, int yes) {
    struct machines *t;
    t = S(w);
    if (action == A_DEL) { if (yes) delete_one(t, t->sel); mark_win_dirty(w); return; }
    if (action >= A_BROWSE && action < A_BROWSE + N_F && yes) {
        ui_ncpy(fv(t, action - A_BROWSE), browse, 127);
        t->fcaret[action - A_BROWSE] = ui_len(fv(t, action - A_BROWSE));
    }
    if (action == A_CBROWSE + CF_ISO && yes) { ui_ncpy(cv(t, CF_ISO), browse, 127); t->ccaret[CF_ISO] = ui_len(cv(t, CF_ISO)); }
    mark_win_dirty(w);
}

// taskmgr.c -- Task Manager, as Windows NT 4 lays it out. docs/c4ix-desktop.md.
//
// Applications: the desktop's windows (End Task, Switch To, New Task).
// Processes: every C4IX task, from utaskinfo, with the CPU share each
// took over the last second (End Process). Performance: a CPU gauge and a
// scrolling history.
//
// CPU use is cpustat.h's, the same as top's: each second, one snapshot of
// the kernel's accounting, and the share of the machine each task took
// since the last. As NT does it, the kernel's own work -- syscalls and
// switches -- is the System row, not the program's, and the idle task and
// the time the machine napped are System Idle Process; CPU usage is
// everything but idle, and the rows add up to 100.

#include "desktop.h"
#include "cpustat.h"

enum { MAXP = CPU_MAXT + 2, HIST = 60, ROW_H = 16, TAB_Y = 6, PAGE_Y = 28, NAMEW = 24 };

struct tasks {
    int tab, sel, top, asel;
    int n;                                     // rows: idle, System, then the tasks
    int id[MAXP], state[MAXP], privs[MAXP], sys[MAXP], cpu[MAXP];
    long long time[MAXP];                      // CPU Time, in cycles
    char name[MAXP * NAMEW];
    struct cpusample s[2];                     // this snapshot and the one before
    int cur;                                   // which of s is the latest
    int usage, hist[HIST], nhist;
    int sampled;
    int uptime0;
};

static struct tasks *S(struct win *w) { return (struct tasks *)w->st; }
static char buf[160];
static char *tabs[3];
static int pct[CPU_MAXT];

static int row(struct tasks *t, int id, char *name, int privs, int state, int sys, int cpu, long long time) {
    int k;
    k = t->n;
    t->id[k] = id; t->privs[k] = privs; t->state[k] = state; t->sys[k] = sys; t->cpu[k] = cpu;
    t->time[k] = time;
    ui_cpy(t->name + k * NAMEW, name);
    t->name[k * NAMEW + NAMEW - 1] = 0;
    ++t->n;
    return k;
}

static void sample(struct tasks *t) {
    struct cpusample *a, *b;
    struct cpustat_task *r;
    int k, sys, nsys;
    long long ktime;

    a = &t->s[t->cur];
    b = &t->s[1 - t->cur];
    cpu_sample(b);
    if (t->sampled) t->usage = cpu_shares(a, b, pct, &sys);
    else { k = 0; while (k < CPU_MAXT) { pct[k] = 0; ++k; } sys = 0; t->usage = 0; }

    // the idle task and System first, as NT lists them; System's time is
    // all the kernel has spent on everyone, the reaped included
    t->n = 0;
    k = cpu_find(b, b->h.idle);
    if (k >= 0) { r = &b->t[k]; row(t, r->id, "System Idle Process", 0, r->state, r->nsyscalls, pct[k], r->user + r->kernel); }
    ktime = b->h.gone_kernel;
    nsys = 0;
    k = 0;
    while (k < b->h.ntasks) {
        if (b->t[k].id != b->h.idle) { ktime = ktime + b->t[k].kernel; nsys = nsys + b->t[k].nsyscalls; }
        ++k;
    }
    row(t, -1, "System", 0, TS_RUNNING, nsys, sys, ktime);
    k = 0;
    while (k < b->h.ntasks) {
        r = &b->t[k];
        if (r->id != b->h.idle) row(t, r->id, r->name, r->privs, r->state, r->nsyscalls, pct[k], r->user);
        ++k;
    }
    t->cur = 1 - t->cur;
    if (t->sampled) {
        if (t->nhist < HIST) { t->hist[t->nhist] = t->usage; ++t->nhist; }
        else { k = 0; while (k < HIST - 1) { t->hist[k] = t->hist[k + 1]; ++k; } t->hist[HIST - 1] = t->usage; }
    }
    t->sampled = 1;
    if (t->sel >= t->n) t->sel = t->n - 1;
}

int tasks_open() {
    int i, k;
    struct win *w;
    // one Task Manager at a time: another request brings it forward
    k = 0;
    while (k < MAXW) { w = win_get(k); if (w->used && w->kind == K_TASKS) { win_raise(k); return k; } ++k; }
    i = win_open(K_TASKS, "Task Manager", 420, 440);
    if (i < 0) return i;
    w = win_get(i);
    S(w)->tab = 1;
    S(w)->uptime0 = __time();
    sample(S(w));
    mark_dirty();
    return i;
}

int tasks_open_perf() {
    int i;
    i = tasks_open();
    if (i >= 0) S(win_get(i))->tab = 2;
    return i;
}

// The sample is taken minimised too, so the history has no gap when the
// window comes back; but a minimised window shows nothing, and redrawing
// the whole scene every second for it was most of what an idle desktop
// cost (docs/c4ix-cpu.md, step 11).
void tasks_tick(struct win *w) {
    sample(S(w));
    if (!w->min) mark_dirty();
}

// The taskbar's CPU meter: the same sample as the Performance tab, kept
// apart from any Task Manager window, so the two agree and neither
// disturbs the other's deltas.
static struct tasks *bar;
int tasks_cpu_usage() {
    if (!bar) { bar = (struct tasks *)ualloc(sizeof(struct tasks)); memset(bar, 0, sizeof(struct tasks)); }
    sample(bar);
    return bar->usage;
}

static char *state_name(int s) {
    if (s == TS_READY) return "Ready";
    if (s == TS_RUNNING) return "Running";
    if (s == TS_ZOMBIE) return "Exited";
    if (s == TS_WAITING) return "Waiting";
    if (s == TS_BLOCKED) return "Blocked";
    if (s == TS_SLEEPING) return "Sleeping";
    return "?";
}

// ---- drawing ----------------------------------------------------------------------

static int widths[6];
static char *labels[6];

static void list_box(int x, int y, int w, int h) {
    d_rect(x, y, w, h, C_WHITE);
    d_bevel(x, y, w, h, B_SUNKEN);
}

static void draw_apps(struct win *me, struct tasks *t, int ox, int oy, int cw, int ch) {
    int k, y, n;
    struct win *w;
    list_box(ox + 8, oy + PAGE_Y + 10, cw - 16, ch - PAGE_Y - 60);
    widths[0] = cw - 20 - 90; widths[1] = 90;
    labels[0] = "Task"; labels[1] = "Status";
    ui_header(ox + 10, oy + PAGE_Y + 12, widths, labels, 2);
    y = oy + PAGE_Y + 32;
    n = 0;
    k = 0;
    while (k < MAXW) {
        w = win_get(k);
        if (w->used && w->kind != K_MSG && w->kind != K_INPUT) {
            if (n == t->asel) d_rect(ox + 12, y - 1, cw - 26, ROW_H, C_SEL);
            d_icon(w->kind, ox + 14, y - 1);
            d_text(ox + 34, y + 1, n == t->asel ? C_WHITE : C_DARK, F_SANS, w->title);
            d_text(ox + 14 + widths[0], y + 1, n == t->asel ? C_WHITE : C_DARK, F_SANS, "Running");
            y = y + ROW_H + 2;
            ++n;
        }
        ++k;
    }
    ui_button(ox + cw - 300, oy + ch - 40, 90, 24, "End Task", 0, 0);
    ui_button(ox + cw - 202, oy + ch - 40, 90, 24, "Switch To", 0, 0);
    ui_button(ox + cw - 104, oy + ch - 40, 90, 24, "New Task...", 0, 0);
}

static void draw_procs(struct tasks *t, int ox, int oy, int cw, int ch) {
    int k, y, x, rows, fg;
    list_box(ox + 8, oy + PAGE_Y + 10, cw - 16, ch - PAGE_Y - 60);
    widths[0] = 120; widths[1] = 40; widths[2] = 36; widths[3] = 74; widths[4] = 60; widths[5] = cw - 20 - 330;
    labels[0] = "Image Name"; labels[1] = "PID"; labels[2] = "CPU"; labels[3] = "CPU Time";
    labels[4] = "Syscalls"; labels[5] = "State";
    ui_header(ox + 10, oy + PAGE_Y + 12, widths, labels, 6);
    rows = (ch - PAGE_Y - 60 - 26) / ROW_H;
    if (t->sel < t->top) t->top = t->sel;
    if (t->sel >= t->top + rows) t->top = t->sel - rows + 1;
    y = oy + PAGE_Y + 32;
    k = t->top;
    while (k < t->n && k < t->top + rows) {
        fg = k == t->sel ? C_WHITE : C_DARK;
        if (k == t->sel) d_rect(ox + 12, y - 1, cw - 26, ROW_H, C_SEL);
        x = ox + 14;
        d_text(x, y + 1, fg, F_SANS, t->name + k * NAMEW); x = x + widths[0];
        buf[0] = 0; if (t->id[k] >= 0) ui_num(buf, t->id[k]);   // System has no PID
        d_text(x + widths[1] - 8 - ui_len(buf) * 6, y + 1, fg, F_SANS, buf); x = x + widths[1];
        ui_num(buf, t->cpu[k]); if (ui_len(buf) < 2) { buf[1] = buf[0]; buf[0] = '0'; buf[2] = 0; }
        d_text(x + widths[2] - 8 - ui_len(buf) * 6, y + 1, fg, F_SANS, buf); x = x + widths[2];
        // CPU time: thousands of cycles, or millions once they pass a billion
        if (t->time[k] >= 1000000000) { ui_num_commas(buf, (int)(t->time[k] / 1000000)); ui_cat(buf, "M"); }
        else { ui_num_commas(buf, (int)(t->time[k] / 1000)); ui_cat(buf, "k"); }
        d_text(x + widths[3] - 8 - ui_len(buf) * 6, y + 1, fg, F_SANS, buf); x = x + widths[3];
        ui_num_commas(buf, t->sys[k]); d_text(x + widths[4] - 8 - ui_len(buf) * 6, y + 1, fg, F_SANS, buf); x = x + widths[4];
        d_text(x + 4, y + 1, fg, F_SANS, state_name(t->state[k]));
        y = y + ROW_H;
        ++k;
    }
    ui_scrollbar(ox + cw - 26, oy + PAGE_Y + 30, ch - PAGE_Y - 82, t->n, rows, t->top);
    ui_button(ox + cw - 104, oy + ch - 40, 90, 24, "End Process", 0, 0);
}

// NT's gauge: green bars on black, lit from the bottom up to the usage.
static void gauge(int x, int y, int w, int h, int pct) {
    int k, n, lit, by;
    d_rect(x, y, w, h, C_DARK);
    n = (h - 22) / 3;
    lit = (n * pct + 50) / 100;
    k = 0;
    while (k < n) {
        by = y + h - 20 - k * 3;
        d_rect(x + 8, by, w / 2 - 10, 2, k < lit ? C_GREEN : 0x004000);
        d_rect(x + w / 2 + 2, by, w / 2 - 10, 2, k < lit ? C_GREEN : 0x004000);
        ++k;
    }
    ui_num(buf, pct); ui_cat(buf, " %");
    d_text(x + w / 2 - ui_len(buf) * 3, y + h - 15, C_GREEN, F_SANS, buf);
}

static void graph(struct tasks *t, int x, int y, int w, int h) {
    int k, gx, px, py, qx, qy;
    d_rect(x, y, w, h, C_DARK);
    k = 1;
    while (k < 5) { d_rect(x, y + (h * k) / 5, w, 1, 0x006000); ++k; }
    gx = (t->nhist * 12) % 12;
    k = w - 1 - gx;
    while (k > 0) { d_rect(x + k, y, 1, h, 0x006000); k = k - 12; }
    if (t->nhist < 2) return;
    k = 1;
    while (k < t->nhist) {
        px = x + w - 1 - (t->nhist - k) * (w - 2) / (HIST - 1);
        qx = x + w - 1 - (t->nhist - 1 - k) * (w - 2) / (HIST - 1);
        py = y + h - 2 - (t->hist[k - 1] * (h - 4)) / 100;
        qy = y + h - 2 - (t->hist[k] * (h - 4)) / 100;
        d_line(px, py, qx, qy, C_GREEN);
        ++k;
    }
}

static void draw_perf(struct tasks *t, int ox, int oy, int cw, int ch) {
    int gw, secs, k;
    struct win *w;
    gw = 90;
    ui_group(ox + 8, oy + PAGE_Y + 8, gw + 16, 130, "CPU Usage");
    gauge(ox + 16, oy + PAGE_Y + 24, gw, 106, t->usage);
    ui_group(ox + gw + 34, oy + PAGE_Y + 8, cw - gw - 42, 130, "CPU Usage History");
    graph(t, ox + gw + 42, oy + PAGE_Y + 24, cw - gw - 58, 106);
    ui_group(ox + 8, oy + PAGE_Y + 150, cw - 16, 90, "Totals");
    d_text(ox + 20, oy + PAGE_Y + 170, C_DARK, F_SANS, "Processes");
    ui_num(buf, t->n); d_text(ox + 150, oy + PAGE_Y + 170, C_DARK, F_SANS, buf);
    d_text(ox + 20, oy + PAGE_Y + 188, C_DARK, F_SANS, "Windows open");
    secs = 0; k = 0; while (k < MAXW) { w = win_get(k); if (w->used) ++secs; ++k; }
    ui_num(buf, secs); d_text(ox + 150, oy + PAGE_Y + 188, C_DARK, F_SANS, buf);
    d_text(ox + 20, oy + PAGE_Y + 206, C_DARK, F_SANS, "Up for (machine seconds)");
    ui_num_commas(buf, (__time() - t->uptime0) / 1000); d_text(ox + 180, oy + PAGE_Y + 206, C_DARK, F_SANS, buf);
}

void tasks_draw(struct win *w, int ox, int oy, int cw, int ch, int focused) {
    struct tasks *t;
    t = S(w);
    tabs[0] = "Applications"; tabs[1] = "Processes"; tabs[2] = "Performance";
    d_rect(ox, oy, cw, ch, C_FACE);
    // the page, then the tabs over its top edge
    d_bevel(ox + 4, oy + PAGE_Y, cw - 8, ch - PAGE_Y - 26, B_RAISED);
    ui_tabs(ox + 8, oy + TAB_Y, tabs, 3, t->tab);
    if (t->tab == 0) draw_apps(w, t, ox, oy, cw, ch - 26);
    else if (t->tab == 1) draw_procs(t, ox, oy, cw, ch - 26);
    else draw_perf(t, ox, oy, cw, ch - 26);
    d_bevel(ox + 2, oy + ch - 22, 120, 20, B_SUNKEN);
    d_bevel(ox + 124, oy + ch - 22, cw - 126, 20, B_SUNKEN);
    ui_cpy(buf, "Processes: "); ui_num(buf + ui_len(buf), t->n);
    d_text(ox + 8, oy + ch - 18, C_DARK, F_SANS, buf);
    ui_cpy(buf, "CPU Usage: "); ui_num(buf + ui_len(buf), t->usage); ui_cat(buf, "%");
    d_text(ox + 130, oy + ch - 18, C_DARK, F_SANS, buf);
}

// ---- input --------------------------------------------------------------------------

// The n'th application window's index, or -1.
static int app_window(int n) {
    int k, i;
    struct win *w;
    i = 0; k = 0;
    while (k < MAXW) {
        w = win_get(k);
        if (w->used && w->kind != K_MSG && w->kind != K_INPUT) { if (i == n) return k; ++i; }
        ++k;
    }
    return 0 - 1;
}

static void end_process(struct win *w) {
    struct tasks *t;
    t = S(w);
    if (t->sel < 0 || t->sel >= t->n) return;
    if (t->privs[t->sel] == 0) {
        msgbox(w, "Unable to Terminate Process", "This is a kernel task; ending it would stop the machine.", 0, A_NONE);
        return;
    }
    ui_cpy(buf, "End process ");
    ui_cat(buf, t->name + t->sel * NAMEW);
    ui_cat(buf, "? Unsaved work in it will be lost.");
    msgbox(w, "Task Manager Warning", buf, 1, A_ENDPROC);
}

void tasks_mouse(struct win *w, int ev, int x, int y, int cw, int ch) {
    struct tasks *t;
    int k, rows, r;
    t = S(w);
    ch = ch - 26;
    if (ev == E_WHEEL && t->tab == 1) {
        t->sel = t->sel + y; if (t->sel < 0) t->sel = 0; if (t->sel >= t->n) t->sel = t->n - 1;
        return;
    }
    rows = (ch - PAGE_Y - 60 - 26) / ROW_H;
    if (ev == E_MOVE && t->tab == 1) {
        r = ui_scroll_drag(cw - 26, PAGE_Y + 30, y);
        if (r >= 0) t->top = r;
        if (r >= 0 && t->sel < t->top) t->sel = t->top;
        if (r >= 0 && t->sel >= t->top + rows) t->sel = t->top + rows - 1;
        return;
    }
    if (ev != E_DOWN && ev != E_DBL) return;
    k = ui_tab_hit(x, y, 8, TAB_Y, tabs, 3);
    if (k >= 0) { t->tab = k; return; }
    if (t->tab == 0) {
        if (ui_hit(x, y, 12, PAGE_Y + 30, cw - 26, ch - PAGE_Y - 80)) {
            t->asel = (y - PAGE_Y - 31) / (ROW_H + 2);
            if (ev == E_DBL && (k = app_window(t->asel)) >= 0) win_raise(k);
        } else if (ui_hit(x, y, cw - 300, ch - 40, 90, 24)) {
            if ((k = app_window(t->asel)) >= 0 && k != win_index(w)) win_close(k);
        } else if (ui_hit(x, y, cw - 202, ch - 40, 90, 24)) {
            if ((k = app_window(t->asel)) >= 0) win_raise(k);
        } else if (ui_hit(x, y, cw - 104, ch - 40, 90, 24))
            inputbox(0, "Create New Task", "Type the name of a program, and C4IX will run it:", "", A_RUN);
    } else if (t->tab == 1) {
        rows = (ch - PAGE_Y - 60 - 26) / ROW_H;
        r = ui_scroll_click(x, y, cw - 26, PAGE_Y + 30, ch - PAGE_Y - 82, t->n, rows, t->top);
        // the selection comes along, or the redraw would scroll back to it
        if (r >= 0) {
            t->top = r;
            if (t->sel < t->top) t->sel = t->top;
            if (t->sel >= t->top + rows) t->sel = t->top + rows - 1;
            return;
        }
        if (ui_hit(x, y, 12, PAGE_Y + 31, cw - 40, rows * ROW_H)) {
            r = t->top + (y - PAGE_Y - 31) / ROW_H;
            if (r < t->n) t->sel = r;
        } else if (ui_hit(x, y, cw - 104, ch - 40, 90, 24)) end_process(w);
    }
}

void tasks_key(struct win *w, int code, int ch, int mods) {
    struct tasks *t;
    t = S(w);
    if (code == KEY_TAB && (mods & M_CTRL)) { t->tab = (t->tab + 1) % 3; return; }
    if (t->tab == 1) {
        if (code == KEY_UP && t->sel > 0) --t->sel;
        else if (code == KEY_DOWN && t->sel < t->n - 1) ++t->sel;
        else if (code == KEY_DEL) end_process(w);
    } else if (t->tab == 0) {
        if (code == KEY_UP && t->asel > 0) --t->asel;
        else if (code == KEY_DOWN && app_window(t->asel + 1) >= 0) ++t->asel;
    }
}

void tasks_answer(struct win *w, int action, int yes) {
    struct tasks *t;
    t = S(w);
    if (action != A_ENDPROC || !yes || t->sel < 0 || t->sel >= t->n) return;
    // interrupt whatever it is running first, then end it
    uintr(t->id[t->sel]);
    ukill(t->id[t->sel], SIGTERM);
    sample(t);
    mark_dirty();
}

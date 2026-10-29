// tools.c -- the desktop's window kinds as window kit classes.
// docs/c4ix-desktop.md.
//
// The desktop's own tools -- Terminal, Files, System Monitor, the Editor,
// About and Settings -- were written before the kit, against a struct
// win and hooks handed the client area's corner. Their hooks are one
// table here (the desktop review's M1, in place of a ladder of kinds in
// each of six places), and one set of adapters makes every kind a kit
// class. The Calculator, Text Editor and Clock are the kit's own classes.
// The calls the tools make -- win_open, msgbox, inputbox -- are the
// kit's, under their old names.

#include "desktop.h"

struct tool {
    void (*draw)(struct win *w, int ox, int oy, int cw, int ch, int focused);
    void (*mouse)(struct win *w, int ev, int x, int y, int cw, int ch);
    void (*key)(struct win *w, int code, int ch, int mods);
    void (*keyup)(struct win *w, int code, int ch, int mods);
    void (*command)(struct win *w, int menu, int item);
    void (*answer)(struct win *w, int action, int yes);
    void (*tick)(struct win *w);              // every pass (a pipe to read)
    void (*second)(struct win *w);            // once a machine second (live numbers)
    void (*drawn)(struct win *w);             // a frame with it in was shown
    void (*close)(struct win *w);             // going: let go of what it holds
    int  (*pointer)(struct win *w, int x, int y, int cw, int ch);
    int  pointer_shape;
};

static struct tool tools[N_KINDS] = {
    [K_TERM]     = { .draw = term_draw, .mouse = term_mouse, .key = term_key, .keyup = term_keyup, .command = term_command,
                     .tick = term_tick, .drawn = term_drawn, .close = term_close, .pointer_shape = PTR_TEXT },
    [K_FILES]    = { .draw = files_draw, .mouse = files_mouse, .key = files_key,
                     .command = files_command, .answer = files_answer },
    [K_TASKS]    = { .draw = tasks_draw, .mouse = tasks_mouse, .key = tasks_key,
                     .answer = tasks_answer, .second = tasks_tick },
    [K_EDIT]     = { .draw = edit_draw, .mouse = edit_mouse, .key = edit_key, .command = edit_command,
                     .answer = edit_answer, .tick = edit_tick, .close = edit_close, .pointer = edit_pointer },
    [K_SETTINGS] = { .draw = settings_draw, .mouse = settings_mouse, .key = settings_key },
    [K_ABOUT]    = { .draw = about_draw }
};
static struct wk_class classes[N_KINDS];
static int ready;

static struct tool *tool(struct win *w) { return &tools[w->kind]; }

// ---- the adapters: a kit class's hooks, calling a tool's ----

static void a_draw(int i, int cw, int ch, int focused) {
    struct win *w;
    if ((w = win_get(i))) tool(w)->draw(w, 0, 0, cw, ch, focused);
}
static void a_mouse(int i, int ev, int x, int y, int b) {
    struct win *w;
    int cw, ch;
    if (!(w = win_get(i))) return;
    wk_size(i, &cw, &ch);
    tool(w)->mouse(w, ev, x, ev == E_WHEEL ? b : y, cw, ch);   // the wheel's steps came as y
}
static void a_key(int i, int down, int code, int ch, int mods) {
    struct win *w;
    if (!(w = win_get(i))) return;
    if (down) tool(w)->key(w, code, ch, mods);
    else if (tool(w)->keyup) tool(w)->keyup(w, code, ch, mods);
}
static void a_command(int i, int id) {
    struct win *w;
    if ((w = win_get(i))) tool(w)->command(w, id / WK_CMD_STEP, id % WK_CMD_STEP);
}
static void a_answer(int i, int dialog, int yes) {
    struct win *w;
    if ((w = win_get(i))) tool(w)->answer(w, dialog, yes);
}
static void a_idle(int i) { struct win *w; if ((w = win_get(i))) tool(w)->tick(w); }
static void a_timer(int i, int id) { struct win *w; if ((w = win_get(i))) tool(w)->second(w); }
static void a_drawn(int i) { struct win *w; if ((w = win_get(i))) tool(w)->drawn(w); }
static void a_destroy(int i) { struct win *w; if ((w = win_get(i))) tool(w)->close(w); }
static int a_pointer(int i, int x, int y) {
    struct win *w;
    int cw, ch;
    if (!(w = win_get(i))) return PTR_ARROW;
    wk_size(i, &cw, &ch);
    return tool(w)->pointer(w, x, y, cw, ch);
}

static void init() {
    int k;
    struct tool *t;
    struct wk_class *c;
    ready = 1;
    k = 0;
    while (k < N_KINDS) {
        t = &tools[k]; c = &classes[k];
        c->kind = k;
        c->pointer_shape = t->pointer_shape;
        if (t->draw) c->draw = a_draw;
        if (t->mouse) c->mouse = a_mouse;
        if (t->key) c->key = a_key;
        if (t->command) c->command = a_command;
        if (t->answer) c->answer = a_answer;
        if (t->tick) c->idle = a_idle;
        if (t->second) c->timer = a_timer;
        if (t->drawn) c->drawn = a_drawn;
        if (t->close) c->destroy = a_destroy;
        if (t->pointer) c->pointer = a_pointer;
        ++k;
    }
}

struct wk_class *kind_class(int kind) {
    if (kind == K_CALC) return &calc_class;
    if (kind == K_NOTE) return &notepad_class;
    if (kind == K_CLOCK) return &clock_class;
    if (!ready) init();
    return kind > 0 && kind < N_KINDS ? &classes[kind] : &classes[K_ABOUT];
}

// ---- the calls the tools make ----

int win_open(int kind, char *title, int w, int h, int size) {
    struct wk_class *c;
    int i;
    c = kind_class(kind);
    c->size = size;
    i = wk_open(c, title, w, h, 0, 0);
    if (i > 0 && kind > 0 && kind < N_KINDS && tools[kind].second) wk_timer(i, 1, 1000);
    return i;
}
void win_close(int i) { wk_close(i); }
void win_title(struct win *w, char *s) { wk_title(w->id, s); }
void win_raise(int i) { wk_raise(i); }
int  win_focused() { int f; f = wk_focus(); return f ? f : 0 - 1; }
void mark_dirty() { wk_redraw_all(); }
void mark_win_dirty(struct win *w) { wk_invalidate(w->id, 0, 0, 0, 0); }
int  now_ms() { return wk_now(); }

void msgbox(struct win *owner, char *title, char *text, int yesno, int action) {
    wk_msgbox(owner ? owner->id : 0, action, title, text, yesno ? WK_YESNO : WK_OK);
}

// The text a box was answered with, copied here as the answer is given:
// each box edits a text of its own while it is open.
static char dtext[WK_PATH];
char *dialog_text() { return dtext; }
void inputbox(struct win *owner, char *title, char *prompt, char *initial, int action) {
    ui_ncpy(dtext, initial, 121);
    wk_inputbox(owner ? owner->id : 0, action, title, prompt, dtext, 121);   // 120 characters
}

void run_command(char *cmd) { term_open(cmd); }
int note_open(char *path) { return notepad_open(path); }

void launch(int kind) {
    if (kind == K_TERM) term_open(0);
    else if (kind == K_FILES) files_open("/");
    else if (kind == K_TASKS) tasks_open();
    else if (kind == K_NOTE) note_open(0);
    else if (kind == K_EDIT) edit_open(0);
    else if (kind == K_CALC) calc_open();
    else if (kind == K_CLOCK) clock_open();
    else if (kind == K_SETTINGS) settings_open();
    else if (kind == K_ABOUT) about_open();
}

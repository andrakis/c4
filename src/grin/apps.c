// apps.c -- grin's applications (docs/grin-plan.md 3.4): the table of the
// window kit it publishes in C4DOS's slot 25, and the applications it
// has loaded through DOS's load, start and unload.
//
// An application's windows are the kit's like any others, but the kit
// must not call the application's class itself: every call into an
// application goes through DOS's catch, so that an exit() in it comes
// back to grin, and in the application's heap generation, so that what
// it allocates is its own. So each class an application opens windows
// of has a proxy here, a class of grin's whose hooks are trampolines;
// the kit holds the proxy, and a trampoline finds the real class and
// calls it under catch. The application never sees a proxy: wk_open,
// wk_find and wk_class_of take and give its own class (the table's
// entries for them are this file's).
//
// An application that exits from a callback is dead: nothing more is
// called in it, and at the top of grin's loop (apps_reap, never inside
// a callback, where the kit and the application are still on the stack)
// its windows are closed and it is unloaded. An application whose last
// window has gone is unloaded there too.

#include "shell.h"
#include "grin.h"
#include "gkit.h"

extern int *__c4dos_api;              // bare.c's: C4DOS's table

enum { SLOT_VERSION = 5, SLOT_LOAD = 20, SLOT_START = 21, SLOT_UNLOAD = 22, SLOT_CATCH = 23,
       SLOT_SHELL = 25, SLOT_GEN = 26, SLOT_SETGEN = 27 };
enum { APPS = 16, PCLASSES = 32, ARGS = 8 };

static int dos_v4() { return __c4dos_api && __c4dos_api[SLOT_VERSION] >= 4; }
static int dos1(int slot, int a) {
    int (*f)(int);
    if (!dos_v4() || !__c4dos_api[slot]) return 0 - 1;
    f = (int (*)(int))__c4dos_api[slot];
    return f(a);
}
static int dos3(int slot, int a, int b, int c) {
    int (*f)(int, int, int);
    if (!dos_v4() || !__c4dos_api[slot]) return 0 - 1;
    f = (int (*)(int, int, int))__c4dos_api[slot];
    return f(a, b, c);
}

// an application: its image's handle (0, a free entry), its generation,
// and its program's name; numbered 1 up (index + 1)
struct app { int h, gen, dead; char name[WK_NAME]; };
static struct app apps[APPS];
// a proxy: the class the kit holds for an application's class (app 0: free)
struct pclass { struct wk_class c; struct wk_class *real; int app; };
static struct pclass pcs[PCLASSES];
static int cur;                       // the application whose code is running, or 0
static int hello;                     // an image being loaded said it is an application
static int grin_gen;
static struct grin_kit kit;

static struct pclass *pclass_of(int w) {
    struct wk_class *c;
    int i;
    if (!(c = wk_class_of(w))) return 0;
    i = 0;
    while (i < PCLASSES) { if (pcs[i].app && &pcs[i].c == c) return &pcs[i]; ++i; }
    return 0;
}

// the application window w belongs to, or 0 (grin's own, or a dialog)
static int app_of(int w) { struct pclass *p; p = pclass_of(w); return p ? p->app : 0; }

// ---- calls into an application -------------------------------------------------

enum { H_OPEN, H_DRAW, H_MOUSE, H_KEY, H_COMMAND, H_TIMER, H_ANSWER, H_CLOSE, H_DESTROY,
       H_POINTER, H_IDLE, H_DRAWN };
struct call { struct wk_class *c; int hook, w, a, b, x, y; };

// under catch: one hook of the real class
static int thunk(int arg) {
    struct call *k;
    struct wk_class *c;
    k = (struct call *)arg;
    c = k->c;
    if (k->hook == H_DRAW) c->draw(k->w, k->a, k->b, k->x);
    else if (k->hook == H_TIMER) c->timer(k->w, k->a);
    else if (k->hook == H_MOUSE) c->mouse(k->w, k->a, k->b, k->x, k->y);
    else if (k->hook == H_KEY) c->key(k->w, k->a, k->b, k->x, k->y);
    else if (k->hook == H_COMMAND) c->command(k->w, k->a);
    else if (k->hook == H_ANSWER) c->answer(k->w, k->a, k->b);
    else if (k->hook == H_OPEN) c->open(k->w, (char *)k->a);
    else if (k->hook == H_CLOSE) return c->close(k->w);
    else if (k->hook == H_DESTROY) c->destroy(k->w);
    else if (k->hook == H_POINTER) return c->pointer(k->w, k->a, k->b);
    else if (k->hook == H_IDLE) c->idle(k->w);
    else if (k->hook == H_DRAWN) c->drawn(k->w);
    return 0;
}

// The hook for window w, in its application's generation, under catch.
// An application that is dead hears nothing more, and one that exits
// here is dead from now on: dflt is the answer then.
static int call(int w, int hook, int a, int b, int x, int y, int dflt) {
    struct pclass *p;
    struct app *ap;
    struct call k;
    int old, oldcur, r, exited;
    if (!(p = pclass_of(w))) return dflt;
    ap = &apps[p->app - 1];
    if (ap->dead) return dflt;
    k.c = p->real; k.hook = hook; k.w = w; k.a = a; k.b = b; k.x = x; k.y = y;
    oldcur = cur; cur = p->app;
    old = dos1(SLOT_SETGEN, ap->gen);
    exited = 0;
    r = dos3(SLOT_CATCH, (int)thunk, (int)&k, (int)&exited);
    if (old >= 0) dos1(SLOT_SETGEN, old);
    cur = oldcur;
    if (exited) { ap->dead = 1; return dflt; }
    return r;
}

static void t_open(int w, char *arg) { call(w, H_OPEN, (int)arg, 0, 0, 0, 0); }
static void t_draw(int w, int cw, int ch, int focused) { call(w, H_DRAW, cw, ch, focused, 0, 0); }
static void t_mouse(int w, int ev, int x, int y, int b) { call(w, H_MOUSE, ev, x, y, b, 0); }
static void t_key(int w, int down, int code, int ch, int mods) { call(w, H_KEY, down, code, ch, mods, 0); }
static void t_command(int w, int id) { call(w, H_COMMAND, id, 0, 0, 0, 0); }
static void t_timer(int w, int id) { call(w, H_TIMER, id, 0, 0, 0, 0); }
static void t_answer(int w, int dialog, int result) { call(w, H_ANSWER, dialog, result, 0, 0, 0); }
static int  t_close(int w) { return call(w, H_CLOSE, 0, 0, 0, 0, 1); }
static void t_destroy(int w) { call(w, H_DESTROY, 0, 0, 0, 0, 0); }
static int  t_pointer(int w, int x, int y) { return call(w, H_POINTER, x, y, 0, 0, PTR_ARROW); }
static void t_idle(int w) { call(w, H_IDLE, 0, 0, 0, 0, 0); }
static void t_drawn(int w) { call(w, H_DRAWN, 0, 0, 0, 0, 0); }

// the proxy for application n's class r, made to match it (an
// application sets its class's size, and may change a hook, before it
// opens each window)
static struct pclass *pclass_get(int n, struct wk_class *r) {
    struct pclass *p;
    int i, f;
    p = 0; f = 0 - 1;
    i = 0;
    while (i < PCLASSES && !p) {
        if (pcs[i].app == n && pcs[i].real == r) p = &pcs[i];
        else if (!pcs[i].app && f < 0) f = i;
        ++i;
    }
    if (!p) {
        if (f < 0) return 0;
        p = &pcs[f];
        p->app = n; p->real = r;
    }
    p->c.size = r->size; p->c.kind = r->kind; p->c.pointer_shape = r->pointer_shape;
    p->c.open = 0; p->c.draw = 0; p->c.mouse = 0; p->c.key = 0; p->c.command = 0; p->c.timer = 0;
    p->c.answer = 0; p->c.close = 0; p->c.destroy = 0; p->c.pointer = 0; p->c.idle = 0; p->c.drawn = 0;
    if (r->open) p->c.open = t_open;
    if (r->draw) p->c.draw = t_draw;
    if (r->mouse) p->c.mouse = t_mouse;
    if (r->key) p->c.key = t_key;
    if (r->command) p->c.command = t_command;
    if (r->timer) p->c.timer = t_timer;
    if (r->answer) p->c.answer = t_answer;
    if (r->close) p->c.close = t_close;
    if (r->destroy) p->c.destroy = t_destroy;
    if (r->pointer) p->c.pointer = t_pointer;
    if (r->idle) p->c.idle = t_idle;
    if (r->drawn) p->c.drawn = t_drawn;
    return p;
}

// ---- the table's own entries: what takes or gives a class -----------------------

static int g_open(struct wk_class *c, char *title, int w, int h, int flags, char *arg) {
    struct pclass *p;
    if (!cur) return wk_open(c, title, w, h, flags, arg);
    if (!(p = pclass_get(cur, c))) return 0 - 1;
    return wk_open(&p->c, title, w, h, flags, arg);
}

static int g_find(struct wk_class *c) {
    int i;
    if (!cur) return wk_find(c);
    i = 0;
    while (i < PCLASSES) { if (pcs[i].app == cur && pcs[i].real == c) return wk_find(&pcs[i].c); ++i; }
    return 0;
}

static struct wk_class *g_class_of(int w) {
    struct pclass *p;
    if ((p = pclass_of(w))) return p->real;
    return wk_class_of(w);
}

static void g_hello() { hello = 1; }

// ---- starting one ------------------------------------------------------------------

static char line[WK_PATH], path[WK_PATH + 8], msg[WK_PATH + 96];
static char *argv[ARGS + 1];

static int up(int c) { return c >= 'a' && c <= 'z' ? c - 32 : c; }
static int same(char *a, char *b) {
    while (*a && up(*a) == up(*b)) { ++a; ++b; }
    return !*a && !*b;
}

static void say(char *a, char *b, char *c) {
    msg[0] = 0;
    ui_ncat(msg, a, WK_PATH + 96); ui_ncat(msg, b, WK_PATH + 96); ui_ncat(msg, c, WK_PATH + 96);
    wk_msgbox(0, 0, "grin", msg, WK_OK);
}

// Starts a command line as an application: its program loaded once, and
// main called again for every launch after the first. 0 if it started;
// -1, with a message box saying why, if not.
int grin_run(char *cmdline) {
    int i, n, h, argc, first, st, oldcur;
    char *s;
    struct app *a;
    if (!dos_v4()) return 0 - 1;
    if (!ui_ncpy(line, cmdline, WK_PATH)) { say("That command line is too long.", "", ""); return 0 - 1; }
    // the words: the program, then its arguments
    argc = 0; s = line;
    while (*s && argc < ARGS) {
        while (*s == ' ') { *s = 0; ++s; }
        if (!*s) break;
        argv[argc] = s; ++argc;
        while (*s && *s != ' ') ++s;
    }
    if (*s) *s = 0;
    argv[argc] = 0;
    if (!argc) return 0 - 1;
    // the program's file: as typed if it has an extension, else with .c4r
    ui_ncpy(path, argv[0], WK_PATH);
    s = path; n = 0;
    while (*s) { if (*s == '.') n = 1; else if (*s == '/' || *s == '\\' || *s == ':') n = 0; ++s; }
    if (!n) ui_ncat(path, ".c4r", WK_PATH + 8);
    // loaded already?
    n = 0; i = 0;
    while (i < APPS && !n) { if (apps[i].h && !apps[i].dead && same(apps[i].name, path)) n = i + 1; ++i; }
    first = !n;
    if (first) {
        i = 0;
        while (i < APPS && apps[i].h) ++i;
        if (i == APPS) { say("Too many applications are running to start ", argv[0], "."); return 0 - 1; }
        hello = 0;
        if (!(h = dos1(SLOT_LOAD, (int)path))) { say("Cannot start ", argv[0], ": there is no such program, or it would not load."); return 0 - 1; }
        // not an application: a DOS program, which DOS runs again full
        // screen, from the top of the loop (grin_full)
        if (!hello) {
            dos1(SLOT_UNLOAD, h);
            return grin_full(cmdline);
        }
        n = i + 1;
        a = &apps[i];
        a->h = h; a->dead = 0;
        a->gen = dos1(SLOT_GEN, h);
        ui_ncpy(a->name, path, WK_NAME);
    }
    a = &apps[n - 1];
    oldcur = cur; cur = n;
    st = dos3(SLOT_START, a->h, argc, (int)argv);
    cur = oldcur;
    // main declined to stay (or exited): its windows go, and so does it
    if (first && st != GRIN_RESIDENT) a->dead = 1;
    return 0;
}

// ---- the top of the loop: the dead and the windowless unloaded ------------------

static int windows_of(int n) {
    struct win *w;
    int k;
    k = 0;
    w = win_first();
    while (w) { if (w->used && app_of(w->id) == n) ++k; w = w->next; }
    return k;
}

static void unload(int n) {
    int i;
    if (dos1(SLOT_UNLOAD, apps[n - 1].h) < 0) return;   // still on the stack: the next pass
    apps[n - 1].h = 0; apps[n - 1].dead = 0; apps[n - 1].name[0] = 0;
    i = 0;
    while (i < PCLASSES) { if (pcs[i].app == n) { pcs[i].app = 0; pcs[i].real = 0; } ++i; }
}

// A dead application's windows closed, its dialogs first (the kit's
// dialogs it asked for, which would answer no one); then every
// application without a window unloaded.
void apps_reap() {
    struct win *w;
    int i, n;
    i = 0;
    while (i < APPS) {
        n = i + 1;
        if (apps[i].h && apps[i].dead) {
            w = win_first();
            while (w) { if (w->used && w->dialog && w->owner && app_of(w->owner) == n) wk_close(w->id); w = w->next; }
            w = win_first();
            while (w) { if (w->used && app_of(w->id) == n) wk_close(w->id); w = w->next; }
        }
        if (apps[i].h && !windows_of(n)) unload(n);
        ++i;
    }
}

int grin_heapgen() { return grin_gen > 0 ? grin_gen : 0; }

// how many are loaded (Help > About says)
int apps_count() {
    int i, k;
    k = 0; i = 0;
    while (i < APPS) { if (apps[i].h) ++k; ++i; }
    return k;
}

// ---- the table -------------------------------------------------------------------

// grin's start: the table filled and published in slot 25
void apps_init() {
    kit.magic = GRIN_MAGIC; kit.version = GRIN_KIT_VERSION;
    grin_gen = dos1(SLOT_GEN, 0);
    kit.gen = grin_gen > 0 ? grin_gen : 0;
    kit.env = &wk;
    kit.hello = g_hello;
#define K(r, n, p, a) kit.n = n;
#define V(n, p, a) kit.n = n;
#include "kitfns.h"
#undef K
#undef V
    kit.wk_open = g_open;
    kit.wk_find = g_find;
    kit.wk_class_of = g_class_of;
    if (dos_v4()) __c4dos_api[SLOT_SHELL] = (int)&kit;
}

// grin's end, after every window has closed: every application unloaded,
// and the table taken back
void apps_end() {
    int i;
    i = 0;
    while (i < APPS) { if (apps[i].h) unload(i + 1); ++i; }
    if (dos_v4() && __c4dos_api[SLOT_SHELL] == (int)&kit) __c4dos_api[SLOT_SHELL] = 0;
}

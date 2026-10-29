// edit.c -- the IDE: a text editor, modeless or with VIM's modes, that
// builds what it edits. docs/c4ix-desktop.md.
//
// The text is an array of lines, each its own allocation, so a file is
// as big as memory allows and an edit moves one line, not the file.
// Everything that changes text goes through four primitives -- insert a
// line, delete a line, insert text at a position, delete a range -- and
// undo is a snapshot of the whole text before each change, which is
// simple and, for the files this machine holds, cheap.
//
// Two ways of typing, chosen in its Settings (View > Settings, kept in
// /etc/ide.cfg with the colour scheme). Traditional, the default, is Text
// Editor's: typing always types, Shift with the moving keys or a drag of
// the mouse selects, and typing replaces the selection; Ctrl+X, C and V
// (and Shift+Del, Ctrl+Ins, Shift+Ins) cut, copy and paste, Ctrl+A selects
// everything, Ctrl+Z and Ctrl+Y undo and redo, and Esc opens the command
// line below (:w, :make and the rest). VIM mode (":set vim" for one
// window, ":set novim" back):
//   Normal   h j k l w b e W B E 0 ^ $ gg G { } % f t F T ; ,  with counts
//            i a I A o O  x X D C s S r J ~  p P  u Ctrl-R  .  n N *
//            d c y > <  with any motion, doubled for lines, and iw aw
//            i( a( i{ a{ i" a" as text objects;  v V for Visual
//   Insert   typing, Enter (keeping the indent), Backspace, Tab, Esc
//   Command  :w :q :q! :wq :x :e FILE :N :$ :s/a/b/ :%s/a/b/g
//            :make :run [CMD] :cn :cp :copen :cclose :set nu|nonu|vim|novim
//            /text ?text
// Arrow keys, Home/End, PgUp/PgDn, Ctrl+Home/End and Ctrl+Left/Right
// work in every mode, Insert included; Shift+arrows extend a Visual
// selection. Ctrl+S saves from any mode.
//
// Building. :make (or F5) writes the file and runs b4ke over pipes into
// the pane at the bottom: on the nearest BUILD.B4K above the file if
// there is one, or else, for a .c file, a build file of its own that
// compiles it with c4sc and links it with libc4ix into /tmp. Lines that
// name a place -- "FILE:N:", c4lc's "line N:", c4cc's "N:" -- are errors:
// :cn and :cp (F8, Shift+F8), or a double click, go to them. :run (F6)
// runs what was built in a new Terminal: the .c file's image, or
// the command a BUILD.B4K names with "SET RUN command".
//
// Long lines fold onto the rows below (Word Wrap, on to start with), or,
// with :set nowrap (View > Word Wrap), scroll across under a bar; j and k
// move by line either way. The
// build pane scrolls both ways, and its Cancel button (or Build > Cancel)
// stops a build: what b4ke is running first, then b4ke.
//
// Files. A pane down the left is the disk as a tree, closed folders to
// start with: a click opens or closes a folder, and opens a file in the
// editor. Opening a file opens the folders on the way to it. Ctrl+B,
// View > Files or :Ex shows and hides it.
//
// Colours. The text is coloured by what the file's name says it is: C and
// headers, JavaScript, Lisp, Forth, shell scripts and Makefiles, and B4KE
// build files. :syntax off (or View > Syntax Colouring) turns it off. The
// colours are a scheme's, chosen in Settings: Blue, after VIM's `blue`
// (the default), or Classic, black on white.
//
// Edit's Cut, Copy and Paste go through the desktop's clipboard (the
// kit's, which every window shares): in VIM mode they take a Visual
// selection. A file that looks binary -- by its name (.c4r, .wad, .tgz
// and the like) or a NUL in its first bytes -- is opened only once asked.

#include "desktop.h"

enum { ECW = 7, ECH = 15, EFONT = 12, UNDO_MAX = 32, BROWS = 8, BLOG_MAX = 300,
       ERR_MAX = 100, REC_MAX = 3000, KEY_F4 = 115, KEY_F6 = 117, KEY_F8 = 119 };
// the file pane: its width, its header, a row; a node's flags; how many
// entries of one folder it lists
enum { FW = 180, FHDR = 20, FROW = 16, F_DIR = 1, F_OPEN = 2, F_KIDS = 400, FNAME = 64 };
// the languages it colours, and the colours
enum { L_TEXT = 0, L_C = 1, L_JS = 2, L_LISP = 3, L_FORTH = 4, L_SH = 5, L_B4K = 6, HL_MAX = 1024 };
// what each character is, for the colouring; the scheme's colour for it
// is ec[that] (below)
enum { HC_TEXT = 0, HC_KW = 1, HC_TYPE = 2, HC_STR = 3, HC_COM = 4, HC_NUM = 5, HC_PRE = 6 };
enum { M_NORMAL = 0, M_INSERT = 1, M_VISUAL = 2, M_VLINE = 3, M_CMD = 4 };
// Edit's items, and the answer this window asks for itself
enum { E_CUT = 0, E_COPY = 1, E_PASTE = 2, E_ALL = 4 };
enum { A_BINARY = 40 };

// The colour schemes: the text's colours by HC_*, then the rest. A gutter
// number colour of SHADOW is the desktop scheme's shadow.
enum { SC_BG = 7, SC_GUT = 8, SC_GUTFG = 9, SC_SEL = 10, SC_TILDE = 11, SC_CUR = 12, SC_CURFG = 13, SC_N = 14,
       N_SCHEMES = 2, SHADOW = 0x1000000 };
static char *scheme_names[N_SCHEMES] = { "Blue (after VIM's blue)", "Classic (black on white)" };
static int scheme_cols[N_SCHEMES * SC_N] = {
    // Blue: VIM's `blue`, gold on dark blue
    0xffd700, 0xffffff, 0x87d7ff, 0x87ffaf, 0xd7d7af, 0xff87af, 0x87ffff,
    0x000087, 0x00005f, 0xffff87, 0x5f87d7, 0x5f87ff, 0xffd700, 0x000087,
    // Classic: the IDE's first colours
    0x000000, 0x0000b0, 0x007878, 0xa01818, 0x2a8a2a, 0x905000, 0x8020a0,
    0xffffff, 0xe8e8e8, SHADOW, 0xa8c8ff, 0x6060c0, 0x000000, 0xffffff };
// Settings: the scheme and the input mode (VIM's or not), for every IDE
// window, kept in /etc/ide.cfg
static int ide_scheme, ide_vim, ide_loaded;
static int *ec;                       // the scheme in use: ec[HC_KW] and the rest

struct edit {
    char path[128];
    char **ln; int *len; int *cap; int nl, maxl;   // the text
    int cy, cx, want, top, left;                   // the cursor, its column to keep, the view
    int mode, vim, nu, modified;
    int vy, vx;                                    // where a Visual selection started
    char cmd[160]; int cmdn, cmdkind;              // the : / ? line being typed
    int count, op, opcount, pre;                   // a Normal command half typed
    char msg[120];
    char search[64]; int sdir;
    int fch, fkind;                                // the last f t F T, for ; and ,
    char *reg; int regn, regline;                  // the one register
    char **utext; int *ulen; int *ucy; int *ucx; int un;   // undo
    char **rtext; int *rlen; int *rcy; int *rcx; int rn;   // redo
    int *rec; int recn, recording, changed;        // the keys of the change being made
    int *dot; int dotn, replaying;                 // ... and of the last one, for '.'
    int pid, out_r, building, bstatus, bshown, btop, bsel;   // the build
    char **blog; int blogn;
    char bpart[256]; int bpartn;
    char bctx[128];                                // the source file the last command named
    char **epath; int *eline; int *elog; int en, ecur;
    char bdir[128]; char runcmd[160];
    int rows, cols, gw;                            // the view's size, as last drawn
    char **fpath; int *fdepth; int *fflag;         // the file pane: a node's path, depth, F_ flags
    int fn, fcap, ftop, fshown, ffollow;
    int hver, nosyn;                               // a count of changes; colouring off
    int hl_line, hl_ver, hl_lang, hl_state;        // the colouring state at the top, cached
    int wrap;                                      // long lines fold onto the rows below
    int lcx, lcy;                                  // the cursor at the last draw: the view follows it when it moves
    int tah, ph, hb, maxw;                         // the text area's height, the build pane's, a bar across, the longest line
    int bleft, bmaxw, bcols, bcancel;              // the build pane across; a build being cancelled
    int fleft;                                     // the file pane across, in 6-pixel steps
    int tsel, tlast;                               // Traditional: a selection from (vy, vx); the last key typed text
    int drag;                                      // the mouse is selecting
    char ask[WK_PATH];                             // a file box's path, in and out
    char pend[128];                                // a binary file waiting to be opened, once asked
};

static struct edit *E(struct win *w) { return (struct edit *)w->st; }
static char sb[512];
static struct wk_menu edit_menus[5];
static char *edit_file_items[7];
static char *edit_edit_items[6];
static char *edit_build_items[7];
static char *edit_view_items[6];
static char *edit_help_items[2];
// what Open and Save As offer: the file box's types
static char *ide_filter = "All Files (*)|*|C Source (*.c;*.h)|*.c;*.h|Build Files (*.b4k)|*.b4k|Scripts (*.sh;*.js;*.lisp;*.fs)|*.sh;*.js;*.mjs;*.lisp;*.scm;*.fs;*.f;*.4th|Text (*.txt;*.md)|*.txt;*.md";

static int e_isspace(int c) { return c == ' ' || c == 9 || c == 10 || c == 13; }
static int e_isword(int c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}
// 0 space, 1 word, 2 punctuation; WORD motions count punctuation as word
static int e_class(int c, int big) {
    if (e_isspace(c)) return 0;
    if (big || e_isword(c)) return 1;
    return 2;
}

static void e_say(struct edit *e, char *s) { ui_cpy(e->msg, s); }

// ---- lines -------------------------------------------------------------------------

static void e_lines_reserve(struct edit *e, int need) {
    char **nln;
    int *nlen, *ncap;
    int m, i;
    if (need <= e->maxl) return;
    m = e->maxl ? e->maxl : 64;
    while (m < need) m = m * 2;
    nln = (char **)ualloc(m * sizeof(int));
    nlen = (int *)ualloc(m * sizeof(int));
    ncap = (int *)ualloc(m * sizeof(int));
    i = 0;
    while (i < e->nl) { nln[i] = e->ln[i]; nlen[i] = e->len[i]; ncap[i] = e->cap[i]; ++i; }
    if (e->ln) { free(e->ln); free(e->len); free(e->cap); }
    e->ln = nln; e->len = nlen; e->cap = ncap; e->maxl = m;
}

// room in line Y for NEED characters
static void e_reserve(struct edit *e, int y, int need) {
    char *nb;
    int c;
    if (e->cap[y] >= need + 1) return;
    c = e->cap[y] * 2;
    if (c < 16) c = 16;
    while (c < need + 1) c = c * 2;
    nb = (char *)ualloc(c);
    if (e->len[y] > 0) memcpy(nb, e->ln[y], e->len[y]);
    free(e->ln[y]);
    e->ln[y] = nb;
    e->cap[y] = c;
}

static void e_insline(struct edit *e, int at, char *s, int n) {
    int i, c;
    e_lines_reserve(e, e->nl + 1);
    i = e->nl;
    while (i > at) { e->ln[i] = e->ln[i - 1]; e->len[i] = e->len[i - 1]; e->cap[i] = e->cap[i - 1]; --i; }
    c = n + 1 < 16 ? 16 : n + 1;
    e->ln[at] = (char *)ualloc(c);
    if (n > 0) memcpy(e->ln[at], s, n);
    e->len[at] = n;
    e->cap[at] = c;
    ++e->nl;
    e->modified = 1; e->changed = 1; ++e->hver;
}

static void e_delline(struct edit *e, int at) {
    int i;
    if (at < 0 || at >= e->nl) return;
    free(e->ln[at]);
    i = at;
    while (i < e->nl - 1) { e->ln[i] = e->ln[i + 1]; e->len[i] = e->len[i + 1]; e->cap[i] = e->cap[i + 1]; ++i; }
    --e->nl;
    if (e->nl == 0) e_insline(e, 0, "", 0);
    e->modified = 1; e->changed = 1; ++e->hver;
}

static void e_clear(struct edit *e) {
    int i;
    i = 0;
    while (i < e->nl) { free(e->ln[i]); ++i; }
    e->nl = 0;
}

// The text, split into lines; a final newline makes no empty line after it.
static void e_settext(struct edit *e, char *s, int n) {
    int i, start;
    e_clear(e);
    start = 0;
    i = 0;
    while (i < n) {
        if (s[i] == 10) { e_insline(e, e->nl, s + start, i - start); start = i + 1; }
        ++i;
    }
    if (start < n) e_insline(e, e->nl, s + start, n - start);
    if (e->nl == 0) e_insline(e, 0, "", 0);
}

// The text as one buffer, a newline after every line; *PN its length.
static char *e_gettext(struct edit *e, int *pn) {
    char *b;
    int i, n, o;
    n = 0;
    i = 0;
    while (i < e->nl) { n = n + e->len[i] + 1; ++i; }
    b = (char *)ualloc(n + 1);
    o = 0;
    i = 0;
    while (i < e->nl) {
        if (e->len[i] > 0) memcpy(b + o, e->ln[i], e->len[i]);
        o = o + e->len[i];
        b[o++] = 10;
        ++i;
    }
    if (e->nl == 1 && e->len[0] == 0) o = 0;    // one empty line is an empty file
    b[o] = 0;
    *pn = o;
    return b;
}

// the character at (Y,X); the end of a line is its newline
static int e_char(struct edit *e, int y, int x) {
    if (y < 0 || y >= e->nl) return 10;
    return x < e->len[y] ? e->ln[y][x] & 255 : 10;
}

// The text from (Y0,X0) up to, not including, (Y1,X1).
static char *e_range(struct edit *e, int y0, int x0, int y1, int x1, int *pn) {
    char *b;
    int n, y, o, a, z;
    n = 0;
    y = y0;
    while (y <= y1) {
        a = y == y0 ? x0 : 0;
        z = y == y1 ? x1 : e->len[y] + 1;
        if (z > a) n = n + z - a;
        ++y;
    }
    b = (char *)ualloc(n + 1);
    o = 0;
    y = y0;
    while (y <= y1) {
        a = y == y0 ? x0 : 0;
        z = y == y1 ? x1 : e->len[y] + 1;
        while (a < z) { b[o++] = a < e->len[y] ? e->ln[y][a] : 10; ++a; }
        ++y;
    }
    b[o] = 0;
    *pn = o;
    return b;
}

// Delete from (Y0,X0) up to, not including, (Y1,X1). X1 past the end of
// its line takes the newline too.
static void e_delrange(struct edit *e, int y0, int x0, int y1, int x1) {
    int tail, k;
    if (y1 >= e->nl) { y1 = e->nl - 1; x1 = e->len[y1]; }
    if (x1 > e->len[y1]) {
        if (y1 + 1 < e->nl) { ++y1; x1 = 0; } else x1 = e->len[y1];
    }
    if (x0 > e->len[y0]) x0 = e->len[y0];
    tail = e->len[y1] - x1;
    e_reserve(e, y0, x0 + tail);
    // within one line this copies leftwards, which a forward copy does safely
    k = 0;
    while (k < tail) { e->ln[y0][x0 + k] = e->ln[y1][x1 + k]; ++k; }
    e->len[y0] = x0 + tail;
    k = y1;
    while (k > y0) { e_delline(e, y0 + 1); --k; }
    e->modified = 1; e->changed = 1; ++e->hver;
}

static int e_ry, e_rx;    // where e_instext left off

// Insert S (N bytes, newlines splitting lines) at (Y,X).
static void e_instext(struct edit *e, int y, int x, char *s, int n) {
    int i, start, tail;
    char *t;
    if (x > e->len[y]) x = e->len[y];
    // cut the rest of the line off, append the pieces, put it back
    tail = e->len[y] - x;
    t = (char *)ualloc(tail + 1);
    if (tail > 0) memcpy(t, e->ln[y] + x, tail);
    e->len[y] = x;
    start = 0;
    i = 0;
    while (i <= n) {
        if (i == n || s[i] == 10) {
            e_reserve(e, y, e->len[y] + i - start);
            if (i > start) memcpy(e->ln[y] + e->len[y], s + start, i - start);
            e->len[y] = e->len[y] + i - start;
            if (i < n) { ++y; e_insline(e, y, "", 0); }
            start = i + 1;
        }
        ++i;
    }
    e_ry = y; e_rx = e->len[y];
    e_reserve(e, y, e->len[y] + tail);
    if (tail > 0) memcpy(e->ln[y] + e->len[y], t, tail);
    e->len[y] = e->len[y] + tail;
    free(t);
    e->modified = 1; e->changed = 1; ++e->hver;
}

static void e_inschar(struct edit *e, int c) {
    char one[2];
    one[0] = c; one[1] = 0;
    e_instext(e, e->cy, e->cx, one, 1);
    e->cy = e_ry; e->cx = e_rx;
}

// ---- undo ---------------------------------------------------------------------------

static void e_snap_push(char **text, int *len, int *cy, int *cx, int *n, char *t, int tn, int y, int x) {
    int i;
    if (*n == UNDO_MAX) {
        free(text[0]);
        i = 0;
        while (i < UNDO_MAX - 1) { text[i] = text[i + 1]; len[i] = len[i + 1]; cy[i] = cy[i + 1]; cx[i] = cx[i + 1]; ++i; }
        --*n;
    }
    text[*n] = t; len[*n] = tn; cy[*n] = y; cx[*n] = x;
    ++*n;
}

static void e_redo_clear(struct edit *e) {
    while (e->rn > 0) { --e->rn; free(e->rtext[e->rn]); }
}

// Before a change: remember the text as it is.
static void e_save_undo(struct edit *e) {
    char *t;
    int n;
    t = e_gettext(e, &n);
    e_snap_push(e->utext, e->ulen, e->ucy, e->ucx, &e->un, t, n, e->cy, e->cx);
    e_redo_clear(e);
}

static void e_undo(struct edit *e, int redo) {
    char *t;
    int n;
    if (redo ? e->rn == 0 : e->un == 0) { e_say(e, redo ? "Already at newest change" : "Already at oldest change"); return; }
    t = e_gettext(e, &n);
    if (redo) {
        e_snap_push(e->utext, e->ulen, e->ucy, e->ucx, &e->un, t, n, e->cy, e->cx);
        --e->rn;
        e_settext(e, e->rtext[e->rn], e->rlen[e->rn]);
        e->cy = e->rcy[e->rn]; e->cx = e->rcx[e->rn];
        free(e->rtext[e->rn]);
    } else {
        e_snap_push(e->rtext, e->rlen, e->rcy, e->rcx, &e->rn, t, n, e->cy, e->cx);
        --e->un;
        e_settext(e, e->utext[e->un], e->ulen[e->un]);
        e->cy = e->ucy[e->un]; e->cx = e->ucx[e->un];
        free(e->utext[e->un]);
    }
    e->modified = 1;
    e_say(e, redo ? "redone" : "undone");
}

// ---- files ---------------------------------------------------------------------------

static void f_reveal(struct edit *e, char *path);
static void f_add(struct edit *e, char *path);

static void e_retitle(struct win *w) {
    struct edit *e;
    int k;
    e = E(w);
    if (e->path[0]) {
        k = ui_len(e->path);
        while (k > 0 && e->path[k - 1] != '/') --k;
        ui_cpy(sb, e->path + k);
    } else ui_cpy(sb, "Untitled");
    if (e->modified) ui_cat(sb, " *");
    ui_cat(sb, " - IDE");
    win_title(w, sb);
}

static int e_load(struct edit *e, char *path) {
    int fd, r, n, cap, st[4];
    char *b, *nb;
    if ((fd = uopen(path, O_RD)) < 0) return 0;
    cap = (ustat(path, st) == 0 && st[1] > 0) ? st[1] + 1 : 4096;
    b = (char *)ualloc(cap + 1);
    n = 0;
    while (1) {
        if (n == cap) { nb = (char *)ualloc(cap * 2 + 1); memcpy(nb, b, n); free(b); b = nb; cap = cap * 2; }
        r = uread(fd, b + n, cap - n);
        if (r <= 0) break;
        n = n + r;
    }
    uclose(fd);
    e_settext(e, b, n);
    free(b);
    ui_ncpy(e->path, path, 128);
    e->cy = 0; e->cx = 0; e->want = 0; e->top = 0; e->left = 0;
    e->modified = 0;
    while (e->un > 0) { --e->un; free(e->utext[e->un]); }
    e_redo_clear(e);
    f_reveal(e, e->path);
    return 1;
}

static int e_save(struct edit *e, char *path) {
    int fd, r, n;
    char *t;
    if (!path[0]) { e_say(e, "No file name"); return 0; }
    if ((fd = uopen(path, O_WR + O_CREATE + O_TRUNCATE)) < 0) { e_say(e, "Cannot write that file"); return 0; }
    t = e_gettext(e, &n);
    r = n > 0 ? write(fd, t, n) : 0;
    uclose(fd);
    free(t);
    if (r != n) { e_say(e, "Write failed"); return 0; }
    if (path != e->path) ui_ncpy(e->path, path, 128);
    e->modified = 0;
    ui_cpy(e->msg, "\"");
    ui_cat(e->msg, path);
    ui_cat(e->msg, "\" written, ");
    ui_num(e->msg + ui_len(e->msg), e->nl);
    ui_cat(e->msg, " lines");
    f_add(e, e->path);
    f_reveal(e, e->path);
    return 1;
}

// Binary files, by their names' ends; then any file with a NUL in its
// first bytes, which text never has.
static char *bin_ext = " .c4r .c4o .c4l .c4s .c4m .tgz .tar .gz .zip .z .wad .wl1 .wl6 .sod .rtl .rts .grp .pak .bin .img .vfs .png .jpg .gif .bmp .ico .wav .mp3 .ogg .mid .voc .exe .com .o .a .so ";
static int e_inwords(char *s, int n, char *list);
static int e_binary(char *p) {
    int i, k, fd, n;
    char ext[12];
    k = ui_len(p);
    i = k;
    while (i > 0 && p[i - 1] != '.' && p[i - 1] != '/') --i;
    if (i > 0 && p[i - 1] == '.' && k - i + 1 < 12) {
        n = 0;
        while (i - 1 + n < k) { ext[n] = p[i - 1 + n]; if (ext[n] >= 'A' && ext[n] <= 'Z') ext[n] = ext[n] + 32; ++n; }
        if (e_inwords(ext, n, bin_ext)) return 1;
    }
    if ((fd = uopen(p, O_RD)) < 0) return 0;
    n = uread(fd, sb, 512);
    uclose(fd);
    i = 0;
    while (i < n) { if (!sb[i]) return 1; ++i; }
    return 0;
}

// A file into the editor: 1 opened, 0 cannot, 2 asked first (a binary
// file, which A_BINARY's answer opens).
static int e_open(struct win *w, char *path, int force) {
    struct edit *e;
    e = E(w);
    if (!force && e_binary(path)) {
        ui_ncpy(e->pend, path, 128);
        ui_cpy(sb, "'");
        ui_ncat(sb, ui_base(path), 200);
        ui_cat(sb, "' does not look like text: it is a binary file, which the IDE cannot show, and would spoil if it saved it. Open it anyway?");
        wk_msgbox(w->id, A_BINARY, "IDE", sb, WK_OKCANCEL);
        return 2;
    }
    return e_load(e, path);
}

// ---- the cursor ------------------------------------------------------------------------

static void e_clamp(struct edit *e) {
    int max;
    if (e->cy >= e->nl) e->cy = e->nl - 1;
    if (e->cy < 0) e->cy = 0;
    max = e->len[e->cy];
    // Normal mode sits ON a character; Insert can sit after the last
    if (e->vim && e->mode != M_INSERT && max > 0) max = max - 1;
    if (e->cx > max) e->cx = max;
    if (e->cx < 0) e->cx = 0;
}

static int e_firstnb(struct edit *e, int y) {
    int x;
    x = 0;
    while (x < e->len[y] && (e->ln[y][x] == ' ' || e->ln[y][x] == 9)) ++x;
    return x;
}

// Step one position forward (dir 1) or back (-1) over the text, the
// newline at each line's end included. 0 at either end of the text.
static int e_step(struct edit *e, int *y, int *x, int dir) {
    if (dir > 0) {
        if (*x < e->len[*y]) { ++*x; return 1; }
        if (*y + 1 >= e->nl) return 0;
        ++*y; *x = 0; return 1;
    }
    if (*x > 0) { --*x; return 1; }
    if (*y == 0) return 0;
    --*y; *x = e->len[*y]; return 1;
}

// w / W: the start of the next word
static void e_word_fwd(struct edit *e, int *y, int *x, int big) {
    int c, sy;
    c = e_class(e_char(e, *y, *x), big);
    if (c) while (e_class(e_char(e, *y, *x), big) == c) { if (!e_step(e, y, x, 1)) return; }
    sy = *y;
    while (e_class(e_char(e, *y, *x), big) == 0) {
        // an empty line is a word of its own
        if (*y != sy && e->len[*y] == 0) return;
        if (!e_step(e, y, x, 1)) return;
    }
}

// e / E: the end of this word or the next
static void e_word_end(struct edit *e, int *y, int *x, int big) {
    int c, ny, nx;
    if (!e_step(e, y, x, 1)) return;
    while (e_class(e_char(e, *y, *x), big) == 0) { if (!e_step(e, y, x, 1)) return; }
    c = e_class(e_char(e, *y, *x), big);
    while (1) {
        ny = *y; nx = *x;
        if (!e_step(e, &ny, &nx, 1)) return;
        if (e_class(e_char(e, ny, nx), big) != c) return;
        *y = ny; *x = nx;
    }
}

// b / B: the start of this word or the previous one
static void e_word_back(struct edit *e, int *y, int *x, int big) {
    int c, py, px;
    if (!e_step(e, y, x, 0 - 1)) return;
    while (e_class(e_char(e, *y, *x), big) == 0) {
        if (e->len[*y] == 0 && *x == 0) return;
        if (!e_step(e, y, x, 0 - 1)) return;
    }
    c = e_class(e_char(e, *y, *x), big);
    while (1) {
        py = *y; px = *x;
        if (!e_step(e, &py, &px, 0 - 1)) return;
        if (e_class(e_char(e, py, px), big) != c) return;
        *y = py; *x = px;
    }
}

// %: the bracket matching the one at or after the cursor on this line
static int e_match(struct edit *e, int *y, int *x) {
    int c, open, close, dir, depth, ty, tx, k;
    k = *x;
    while (k < e->len[*y]) {
        c = e->ln[*y][k];
        if (c == '(' || c == ')' || c == '[' || c == ']' || c == '{' || c == '}') break;
        ++k;
    }
    if (k >= e->len[*y]) return 0;
    c = e->ln[*y][k];
    if (c == '(' || c == ')') { open = '('; close = ')'; }
    else if (c == '[' || c == ']') { open = '['; close = ']'; }
    else { open = '{'; close = '}'; }
    dir = (c == open) ? 1 : 0 - 1;
    ty = *y; tx = k; depth = 0;
    while (1) {
        c = e_char(e, ty, tx);
        if (c == open) depth = depth + dir;
        else if (c == close) depth = depth - dir;
        if (depth == 0) { *y = ty; *x = tx; return 1; }
        if (!e_step(e, &ty, &tx, dir)) return 0;
    }
    return 0;
}

// search for the literal S from just past (or before) the cursor, wrapping
static int e_find(struct edit *e, char *s, int dir, int *py, int *px) {
    int y, x, n, k, tries;
    n = ui_len(s);
    if (!n) return 0;
    y = *py; x = *px;
    tries = 0;
    while (tries <= e->nl + 1) {
        if (dir > 0) {
            ++x;
            if (x > e->len[y] - n) { y = (y + 1) % e->nl; x = 0 - 1; ++tries; continue; }
        } else {
            --x;
            if (x < 0) { y = (y + e->nl - 1) % e->nl; x = e->len[y] - n + 1; ++tries; continue; }
        }
        if (x < 0) continue;
        k = 0;
        while (k < n && e->ln[y][x + k] == s[k]) ++k;
        if (k == n) { *py = y; *px = x; return 1; }
    }
    return 0;
}

// ---- the build pane --------------------------------------------------------------------

static int e_digits(char *s, int *pv) {
    int n, v;
    n = 0; v = 0;
    while (s[n] >= '0' && s[n] <= '9') { v = v * 10 + (s[n] - '0'); ++n; }
    *pv = v;
    return n;
}

// does the file exist? then it is a place an error can point at
static int e_exists(char *p) {
    int st[4];
    return ustat(p, st) == 0 && st[0] == 2;
}

// An error's file: as written if it is there, else against the build's
// directory, the edited file's, or the top of the source tree.
static void e_resolve(struct edit *e, char *p, char *out) {
    int k;
    if (p[0] == '/' || e_exists(p)) { ui_cpy(out, p); return; }
    ui_cpy(out, e->bdir); ui_cat(out, "/"); ui_cat(out, p);
    if (e_exists(out)) return;
    ui_cpy(out, e->path);
    k = ui_len(out);
    while (k > 0 && out[k - 1] != '/') --k;
    out[k] = 0;
    ui_cat(out, p);
    if (e_exists(out)) return;
    ui_cpy(out, "/usr/"); ui_cat(out, p);
    if (e_exists(out)) return;
    ui_cpy(out, p);
}

static void e_add_error(struct edit *e, char *file, int fn, int line, int logline) {
    char name[128];
    if (e->en >= ERR_MAX || fn <= 0 || fn > 120 || line <= 0) return;
    memcpy(name, file, fn);
    name[fn] = 0;
    e->epath[e->en] = (char *)ualloc(128);
    e_resolve(e, name, e->epath[e->en]);
    e->eline[e->en] = line;
    e->elog[e->en] = logline;
    ++e->en;
}

// What a line of build output says: a command b4ke ran (remember the
// source it names), or an error.
static void e_parse(struct edit *e, char *s, int logline) {
    int i, j, k, n, v;
    if (s[0] == '+' && s[1] == ' ') {
        // "+ c4sc ... src/x.c out.c4o": the last word naming a source
        i = 2;
        while (s[i]) {
            while (s[i] == ' ') ++i;
            j = i;
            while (s[j] && s[j] != ' ') ++j;
            if (j - i > 2 && s[j - 2] == '.' && (s[j - 1] == 'c' || s[j - 1] == 'h') && j - i < 120) {
                memcpy(e->bctx, s + i, j - i);
                e->bctx[j - i] = 0;
            }
            i = j;
        }
        return;
    }
    // b4ke's "did not make" and "exited" follow from an error already
    // listed; only when nothing said why are they the error to see
    if (e->en > 0 && s[0] == 'b' && s[1] == '4' && s[2] == 'k' && s[3] == 'e' && s[4] == ':') return;
    // "FILE:N:" anywhere
    i = 0;
    while (s[i]) {
        if (s[i] == ':' && (n = e_digits(s + i + 1, &v)) > 0 && s[i + 1 + n] == ':') {
            j = i;
            while (j > 0 && s[j - 1] != ' ' && s[j - 1] != 9) --j;
            if (i > j) { e_add_error(e, s + j, i - j, v, logline); return; }
        }
        ++i;
    }
    // c4lc's "line N:", against the source the last command named
    i = 0;
    while (s[i]) {
        if (s[i] == 'l' && s[i + 1] == 'i' && s[i + 2] == 'n' && s[i + 3] == 'e' && s[i + 4] == ' ' &&
            (n = e_digits(s + i + 5, &v)) > 0 && e->bctx[0]) {
            e_add_error(e, e->bctx, ui_len(e->bctx), v, logline);
            return;
        }
        ++i;
    }
    // c4cc's "N: WARNING ..." / "N: error ..."
    if ((n = e_digits(s, &v)) > 0 && s[n] == ':' && e->bctx[0]) {
        k = ui_len(e->bctx);
        e_add_error(e, e->bctx, k, v, logline);
    }
}

static void e_logline(struct edit *e, char *s, int n) {
    int i;
    if (e->blogn == BLOG_MAX) {
        // the oldest line goes, and errors pointing into the log move up
        free(e->blog[0]);
        i = 0;
        while (i < BLOG_MAX - 1) { e->blog[i] = e->blog[i + 1]; ++i; }
        --e->blogn;
        i = 0;
        while (i < e->en) { --e->elog[i]; ++i; }
    }
    e->blog[e->blogn] = (char *)ualloc(n + 1);
    memcpy(e->blog[e->blogn], s, n);
    e->blog[e->blogn][n] = 0;
    e_parse(e, e->blog[e->blogn], e->blogn);
    ++e->blogn;
    // the end stays in view, unless the pane was scrolled up to read
    if (e->blogn > BROWS && e->btop >= e->blogn - 1 - BROWS) e->btop = e->blogn - BROWS;
}

static void e_build_clear(struct edit *e) {
    while (e->blogn > 0) { --e->blogn; free(e->blog[e->blogn]); }
    while (e->en > 0) { --e->en; free(e->epath[e->en]); }
    e->ecur = 0 - 1; e->btop = 0; e->bsel = 0 - 1; e->bpartn = 0; e->bctx[0] = 0; e->bleft = 0;
}

// the directory of PATH, into OUT ("/" for a file at the root)
static void e_dirname(char *path, char *out) {
    int k;
    ui_cpy(out, path);
    k = ui_len(out);
    while (k > 0 && out[k - 1] != '/') --k;
    if (k <= 1) { out[0] = '/'; out[1] = 0; return; }
    out[k - 1] = 0;
}

static int e_namech(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'; }

// Does the build file P name the file BASE (as a word: "mandel.c" but
// not "mandel.c4o" or "xmandel.c")?
static int e_names(char *p, char *base) {
    char *t;
    int fd, n, i, m, j, hit;
    if ((fd = uopen(p, O_RD)) < 0) return 0;
    t = (char *)ualloc(65537);
    n = uread(fd, t, 65536);
    uclose(fd);
    if (n < 0) n = 0;
    t[n] = 0;
    m = ui_len(base);
    hit = 0;
    i = 0;
    while (!hit && i + m <= n) {
        j = 0;
        while (j < m && t[i + j] == base[j]) ++j;
        if (j == m && (i == 0 || !e_namech(t[i - 1])) && !e_namech(t[i + m]) && t[i + m] != '.') hit = 1;
        ++i;
    }
    free(t);
    return hit;
}

// The BUILD.B4K that builds PATH, looking up from DIR: its directory
// into OUT. 2: the nearest one that names the file. 1: none does, and
// OUT is the nearest there is -- /usr's own builds the tree, not
// src/tests/mandel.c, so a lone .c file is better built alone (e_make).
// 0: there is none.
static int e_find_build(char *dir, char *path, char *out) {
    char p[160], near[128], *base;
    int k, found;
    base = path + ui_len(path);
    while (base > path && base[0 - 1] != '/') --base;
    ui_cpy(out, dir);
    found = 0;
    while (1) {
        ui_cpy(p, out);
        if (!(p[0] == '/' && p[1] == 0)) ui_cat(p, "/");
        ui_cat(p, "BUILD.B4K");
        if (e_exists(p)) {
            if (e_names(p, base)) return 2;
            if (!found) { ui_cpy(near, out); found = 1; }
        }
        if (out[0] == '/' && out[1] == 0) break;
        k = ui_len(out);
        while (k > 0 && out[k - 1] != '/') --k;
        if (k <= 1) { out[0] = '/'; out[1] = 0; }
        else out[k - 1] = 0;
    }
    if (found) ui_cpy(out, near);
    return found;
}

// A BUILD.B4K may say what to run: "SET RUN command".
static void e_find_run(struct edit *e, char *dir) {
    char p[160];
    char *t;
    int fd, n, i, k;
    e->runcmd[0] = 0;
    ui_cpy(p, dir);
    if (!(p[0] == '/' && p[1] == 0)) ui_cat(p, "/");
    ui_cat(p, "BUILD.B4K");
    if ((fd = uopen(p, O_RD)) < 0) return;
    t = (char *)ualloc(16385);
    n = uread(fd, t, 16384);
    uclose(fd);
    if (n < 0) n = 0;
    t[n] = 0;
    i = 0;
    while (i < n) {
        if ((i == 0 || t[i - 1] == 10) && t[i] == 'S' && t[i + 1] == 'E' && t[i + 2] == 'T' && t[i + 3] == ' ' &&
            t[i + 4] == 'R' && t[i + 5] == 'U' && t[i + 6] == 'N' && t[i + 7] == ' ') {
            k = 0;
            i = i + 8;
            while (i < n && t[i] != 10 && k < 158) { e->runcmd[k++] = t[i]; ++i; }
            e->runcmd[k] = 0;
            break;
        }
        ++i;
    }
    free(t);
}

static void e_make(struct win *w) {
    struct edit *e;
    char dir[128];
    char name[64];
    char **argv;
    int pin[2], pout[2], s0, s1, s2, k, j, fd, n;
    char *bf;
    int bk;
    e = E(w);
    if (e->building) { e_say(e, "A build is already running"); return; }
    if (!e->path[0]) { e_say(e, "Save the file first (:w FILE)"); return; }
    if (e->modified && !e_save(e, e->path)) return;
    e_retitle(w);
    e_dirname(e->path, dir);
    argv = (char **)ualloc(8 * sizeof(int));
    argv[0] = "b4ke";
    bk = e_find_build(dir, e->path, e->bdir);
    if (bk == 2 || (bk == 1 && !ui_ends(e->path, ".c"))) {
        argv[1] = "-C"; argv[2] = e->bdir; argv[3] = 0;
        e_find_run(e, e->bdir);
        k = 3;
    } else if (ui_ends(e->path, ".c")) {
        // no build file names it: a build file of its own, which has cc
        // compile this one file and link it with libc4ix
        k = ui_len(e->path);
        j = k;
        while (j > 0 && e->path[j - 1] != '/') --j;
        memcpy(name, e->path + j, k - j - 2);
        name[k - j - 2] = 0;
        bf = (char *)ualloc(1024);
        ui_cpy(bf, "B4KE2\nDEPS /tmp/"); ui_cat(bf, name); ui_cat(bf, ".c4r : "); ui_cat(bf, e->path);
        ui_cat(bf, "\nRUN cc -O -l c4ix -I "); ui_cat(bf, dir); ui_cat(bf, " -o /tmp/"); ui_cat(bf, name);
        ui_cat(bf, ".c4r "); ui_cat(bf, e->path); ui_cat(bf, "\n");
        n = ui_len(bf);
        fd = uopen("/tmp/edit.b4k", O_WR + O_CREATE + O_TRUNCATE);
        if (fd < 0 || write(fd, bf, n) != n) { if (fd >= 0) uclose(fd); free(bf); e_say(e, "Cannot write /tmp/edit.b4k"); return; }
        uclose(fd);
        free(bf);
        ui_cpy(e->bdir, dir);
        argv[1] = "-C"; argv[2] = e->bdir; argv[3] = "-f"; argv[4] = "/tmp/edit.b4k"; argv[5] = 0;
        ui_cpy(e->runcmd, "/tmp/"); ui_cat(e->runcmd, name); ui_cat(e->runcmd, ".c4r");
        k = 5;
    } else {
        e_say(e, "Nothing to build: no BUILD.B4K above this file, and it is not a .c file");
        return;
    }

    // b4ke on pipes, as the Terminal runs its shell (term.c): our
    // ends are close-on-spawn, its stdin is a pipe nobody writes
    if (upipe(pin) < 0) { e_say(e, "No pipe for the build"); return; }
    if (upipe(pout) < 0) { uclose(pin[0]); uclose(pin[1]); e_say(e, "No pipe for the build"); return; }
    ucloexec(pin[1], 1);
    ucloexec(pout[0], 1);
    s0 = udup(STDIN); s1 = udup(STDOUT); s2 = udup(STDERR);
    ucloexec(s0, 1); ucloexec(s1, 1); ucloexec(s2, 1);
    udup2(pin[0], STDIN);
    udup2(pout[1], STDOUT);
    udup2(pout[1], STDERR);
    e->pid = spawn("/bin/b4ke", k, argv);
    udup2(s0, STDIN); udup2(s1, STDOUT); udup2(s2, STDERR);
    uclose(s0); uclose(s1); uclose(s2);
    uclose(pin[0]); uclose(pout[1]); uclose(pin[1]);
    if (e->pid < 0) { uclose(pout[0]); e_say(e, "Cannot start b4ke"); return; }
    e->out_r = pout[0];
    e_build_clear(e);
    e->building = 1; e->bshown = 1; e->bcancel = 0;
    ui_cpy(sb, "b4ke"); j = 1;
    while (j < k) { ui_cat(sb, " "); ui_cat(sb, argv[j]); ++j; }
    e_logline(e, sb, ui_len(sb));
    e_say(e, "Building...");
}

static void e_goto_error(struct win *w, int i);

// Output waiting in the build's pipe; never blocks (wm.c's loop calls this).
void edit_tick(struct win *w) {
    struct edit *e;
    char buf[512];
    int n, i;
    e = E(w);
    if (!e->building) return;
    while ((n = uavail(e->out_r)) > 0) {
        if (n > 512) n = 512;
        n = uread(e->out_r, buf, n);
        if (n <= 0) break;
        i = 0;
        while (i < n) {
            if (buf[i] == 10 || e->bpartn >= 250) { e_logline(e, e->bpart, e->bpartn); e->bpartn = 0; }
            if (buf[i] != 10 && buf[i] != 13) e->bpart[e->bpartn++] = buf[i];
            ++i;
        }
        mark_win_dirty(w);   // only the build pane moved
    }
    if (n < 0) {
        // every writer has gone: the build is over
        if (e->bpartn) { e_logline(e, e->bpart, e->bpartn); e->bpartn = 0; }
        uclose(e->out_r);
        e->bstatus = uwait(e->pid);
        e->building = 0;
        if (e->bstatus == 0) ui_cpy(e->msg, "Build finished");
        else if (e->bcancel) ui_cpy(e->msg, "Build cancelled");
        else {
            ui_cpy(e->msg, "Build failed: ");
            ui_num(e->msg + ui_len(e->msg), e->en);
            ui_cat(e->msg, e->en == 1 ? " error" : " errors");
            if (e->en > 0) e_goto_error(w, 0);
        }
        mark_dirty();
    }
}

// Stop the build: what b4ke is running first -- a compiler it is waiting
// on would run on without it, and nobody would wait for it -- then b4ke.
// Every one is waited for, so none is left a zombie.
static int binfo[32];
static void e_stop_build(struct edit *e) {
    int kids[8], nk, i;
    if (!e->building) return;
    nk = 0; i = 0;
    while (nk < 8 && utaskinfo(i, binfo)) { if (binfo[1] == e->pid) { kids[nk] = binfo[0]; ++nk; } ++i; }
    i = 0;
    while (i < nk) { uintr(kids[i]); ukill(kids[i], SIGTERM); ++i; }
    ukill(e->pid, SIGTERM);
    uclose(e->out_r);
    i = 0;
    while (i < nk) { uwait(kids[i]); ++i; }
    e->bstatus = uwait(e->pid);
    e->building = 0;
}
static void e_cancel(struct win *w) {
    struct edit *e;
    e = E(w);
    if (!e->building) { e_say(e, "No build is running"); return; }
    e_stop_build(e);
    e->bcancel = 1;
    if (e->bpartn) { e_logline(e, e->bpart, e->bpartn); e->bpartn = 0; }
    e_logline(e, "-- cancelled --", 15);
    ui_cpy(e->msg, "Build cancelled");
    mark_dirty();
}

void edit_close(struct win *w) {
    struct edit *e;
    int i;
    e = E(w);
    e_stop_build(e);
    i = 0;
    while (i < e->fn) { free(e->fpath[i]); ++i; }
    if (e->fpath) { free((char *)e->fpath); free((char *)e->fdepth); free((char *)e->fflag); }
    e->fpath = 0; e->fn = 0;
}

// ---- changes -----------------------------------------------------------------------------

static void e_yank(struct edit *e, int y0, int x0, int y1, int x1, int linewise) {
    if (e->reg) free(e->reg);
    if (linewise) e->reg = e_range(e, y0, 0, y1, e->len[y1] + 1, &e->regn);
    else e->reg = e_range(e, y0, x0, y1, x1, &e->regn);
    e->regline = linewise;
}

static void e_indent(struct edit *e, int y0, int y1, int more) {
    int y, k;
    y = y0;
    while (y <= y1) {
        if (more) { if (e->len[y] > 0) e_instext(e, y, 0, "    ", 4); }
        else {
            k = 0;
            while (k < 4 && k < e->len[y] && e->ln[y][k] == ' ') ++k;
            if (k) e_delrange(e, y, 0, y, k);
        }
        ++y;
    }
}

// OP over a range. LINEWISE: whole lines Y0..Y1. Otherwise (Y0,X0) up
// to (Y1,X1), including X1 when INCL.
static void e_apply(struct edit *e, int op, int y0, int x0, int y1, int x1, int linewise, int incl) {
    int t, ind;
    if (y1 < y0 || (y1 == y0 && x1 < x0)) { t = y0; y0 = y1; y1 = t; t = x0; x0 = x1; x1 = t; }
    if (!linewise && incl) ++x1;
    if (op == 'y') {
        e_yank(e, y0, x0, y1, x1, linewise);
        e->cy = y0; e->cx = linewise ? e->cx : x0;
        return;
    }
    e_save_undo(e);
    if (op == '>' || op == '<') { e_indent(e, y0, y1, op == '>'); e->cy = y0; e->cx = e_firstnb(e, y0); return; }
    e_yank(e, y0, x0, y1, x1, linewise);
    if (linewise) {
        if (op == 'c') {
            ind = e_firstnb(e, y0);
            e_delrange(e, y0, ind, y1, e->len[y1]);
            e->cy = y0; e->cx = ind;
            e->mode = M_INSERT;
            return;
        }
        t = y1 - y0 + 1;
        while (t > 0) { e_delline(e, y0); --t; }
        e->cy = y0;
        e_clamp(e);
        e->cx = e_firstnb(e, e->cy);
        return;
    }
    e_delrange(e, y0, x0, y1, x1);
    e->cy = y0; e->cx = x0;
    if (op == 'c') e->mode = M_INSERT;
}

static void e_put(struct edit *e, int before) {
    int y;
    if (!e->reg || !e->regn) { e_say(e, "Nothing to put"); return; }
    e_save_undo(e);
    if (e->regline) {
        y = before ? e->cy : e->cy + 1;
        if (y >= e->nl) {
            // after the last line: start a new one, then fill it
            e_insline(e, e->nl, "", 0);
            e_instext(e, y, 0, e->reg, e->regn - 1);
        } else {
            e_insline(e, y, "", 0);
            e_instext(e, y, 0, e->reg, e->regn - 1);
        }
        e->cy = y; e->cx = e_firstnb(e, y);
        return;
    }
    e_instext(e, e->cy, before || e->len[e->cy] == 0 ? e->cx : e->cx + 1, e->reg, e->regn);
    e->cy = e_ry; e->cx = e_rx > 0 ? e_rx - 1 : 0;
}

static void e_join(struct edit *e, int n) {
    int x, k;
    e_save_undo(e);
    while (n > 0 && e->cy + 1 < e->nl) {
        x = e->len[e->cy];
        k = e_firstnb(e, e->cy + 1);
        e_delrange(e, e->cy + 1, 0, e->cy + 1, k);
        e_delrange(e, e->cy, x, e->cy + 1, 0);
        if (x > 0 && e->len[e->cy] > x) e_instext(e, e->cy, x, " ", 1);
        e->cx = x;
        --n;
    }
}

// Enter in Insert: split the line, keeping the indent
static void e_newline(struct edit *e) {
    int ind;
    char spaces[64];
    ind = e_firstnb(e, e->cy);
    if (ind > e->cx) ind = e->cx;
    if (ind > 60) ind = 60;
    memset(spaces, ' ', ind);
    e_instext(e, e->cy, e->cx, "\n", 1);
    e->cy = e_ry; e->cx = 0;
    if (ind) { e_instext(e, e->cy, 0, spaces, ind); e->cx = ind; }
}

static void e_open_line(struct edit *e, int above) {
    int ind, y;
    char spaces[64];
    e_save_undo(e);
    ind = e_firstnb(e, e->cy);
    if (ind > 60) ind = 60;
    memset(spaces, ' ', ind);
    y = above ? e->cy : e->cy + 1;
    e_insline(e, y, spaces, ind);
    e->cy = y; e->cx = ind;
    e->mode = M_INSERT;
}

// ---- motions ----------------------------------------------------------------------------

// f t F T: to (or just before) the Nth C on this line
static int e_motion_find(struct edit *e, int kind, int c, int n, int *ty, int *tx, int *incl) {
    int x, dir;
    x = e->cx;
    dir = (kind == 'f' || kind == 't') ? 1 : 0 - 1;
    // repeating t or T from just before a match must not find it again
    if ((kind == 't' || kind == 'T') && x + dir >= 0 && x + dir < e->len[e->cy] && e->ln[e->cy][x + dir] == c) x = x + dir;
    while (n > 0) {
        x = x + dir;
        while (x >= 0 && x < e->len[e->cy] && e->ln[e->cy][x] != c) x = x + dir;
        if (x < 0 || x >= e->len[e->cy]) return 0;
        --n;
    }
    if (kind == 't') --x;
    if (kind == 'T') ++x;
    *ty = e->cy; *tx = x;
    *incl = dir > 0;
    return 1;
}

// The motion a key names, from the cursor, N times. Returns 1 with
// (*ty,*tx) and how an operator takes it; 0 if the key is no motion;
// -1 if it begins one (g, f, t, ...) whose rest is still to come.
static int e_motion(struct edit *e, int code, int ch, int mods, int n,
                    int *ty, int *tx, int *lw, int *incl) {
    int y, x, k, c;
    y = e->cy; x = e->cx;
    *lw = 0; *incl = 0;
    if (mods & M_CTRL) {
        if (code == KEY_HOME) { *ty = 0; *tx = 0; *lw = 1; return 1; }
        if (code == KEY_END) { *ty = e->nl - 1; *tx = 0; *lw = 1; return 1; }
        if (code == KEY_LEFT) { k = n; while (k--) e_word_back(e, &y, &x, 0); *ty = y; *tx = x; return 1; }
        if (code == KEY_RIGHT) { k = n; while (k--) e_word_fwd(e, &y, &x, 0); *ty = y; *tx = x; return 1; }
        if (code == 68 || code == 85) {              // Ctrl-D, Ctrl-U: half a page
            k = e->rows / 2; if (k < 1) k = 1;
            *ty = code == 68 ? y + k : y - k; *tx = x; *lw = 1; return 1;
        }
        return 0;
    }
    if (code == KEY_LEFT || ch == 'h' || code == KEY_BACK) { *ty = y; *tx = x - n < 0 ? 0 : x - n; return 1; }
    if (code == KEY_RIGHT || ch == 'l' || ch == ' ') { *ty = y; *tx = x + n; return 1; }
    if (code == KEY_UP || ch == 'k') { *ty = y - n; *tx = e->want; *lw = 1; return 1; }
    if (code == KEY_DOWN || ch == 'j') { *ty = y + n; *tx = e->want; *lw = 1; return 1; }
    if (code == KEY_PGUP) { k = e->rows > 1 ? e->rows - 1 : 1; *ty = y - k * n; *tx = e->want; *lw = 1; return 1; }
    if (code == KEY_PGDN) { k = e->rows > 1 ? e->rows - 1 : 1; *ty = y + k * n; *tx = e->want; *lw = 1; return 1; }
    if (code == KEY_HOME || ch == '0') { *ty = y; *tx = 0; return 1; }
    if (ch == '^') { *ty = y; *tx = e_firstnb(e, y); return 1; }
    if (code == KEY_END || ch == '$') {
        *ty = y + n - 1; if (*ty >= e->nl) *ty = e->nl - 1;
        *tx = e->len[*ty] > 0 ? e->len[*ty] - 1 : 0; *incl = 1; e->want = 100000; return 1;
    }
    if (ch == 'w' || ch == 'W') { k = n; while (k--) e_word_fwd(e, &y, &x, ch == 'W'); *ty = y; *tx = x; return 1; }
    if (ch == 'b' || ch == 'B') { k = n; while (k--) e_word_back(e, &y, &x, ch == 'B'); *ty = y; *tx = x; return 1; }
    if (ch == 'e' || ch == 'E') { k = n; while (k--) e_word_end(e, &y, &x, ch == 'E'); *ty = y; *tx = x; *incl = 1; return 1; }
    if (ch == 'G') { *ty = e->count ? e->count - 1 : e->nl - 1; *tx = 0; *lw = 1; return 1; }
    if (ch == '}' || ch == '{') {
        k = n;
        while (k--) {
            c = ch == '}' ? 1 : 0 - 1;
            y = y + c;
            while (y > 0 && y < e->nl - 1 && e->len[y] != 0) y = y + c;
        }
        if (y < 0) y = 0;
        if (y >= e->nl) y = e->nl - 1;
        *ty = y; *tx = 0; return 1;
    }
    if (ch == '%') { if (!e_match(e, &y, &x)) return 0; *ty = y; *tx = x; *incl = 1; return 1; }
    if (ch == 'n' || ch == 'N') {
        if (!e->search[0]) { e_say(e, "No previous search"); return 0; }
        k = (ch == 'n') ? e->sdir : 0 - e->sdir;
        if (!e_find(e, e->search, k, &y, &x)) { e_say(e, "Pattern not found"); return 0; }
        *ty = y; *tx = x; return 1;
    }
    if (ch == ';' || ch == ',') {
        if (!e->fkind) return 0;
        k = e->fkind;
        if (ch == ',') k = (k == 'f') ? 'F' : (k == 'F') ? 'f' : (k == 't') ? 'T' : 't';
        return e_motion_find(e, k, e->fch, n, ty, tx, incl);
    }
    if (ch == 'g' || ch == 'f' || ch == 't' || ch == 'F' || ch == 'T') { e->pre = ch; return 0 - 1; }
    return 0;
}

// iw aw i( a( i{ a{ i" a" i' a': the range a text object covers
static int e_textobj(struct edit *e, int inner, int c, int *y0, int *x0, int *y1, int *x1) {
    int y, x, k, open, close, depth, cls, sy, sx;
    y = e->cy; x = e->cx;
    if (c == 'w' || c == 'W') {
        if (e->len[y] == 0) return 0;
        cls = e_class(e_char(e, y, x), c == 'W');
        sx = x;
        while (sx > 0 && e_class(e->ln[y][sx - 1], c == 'W') == cls) --sx;
        k = x;
        while (k + 1 < e->len[y] && e_class(e->ln[y][k + 1], c == 'W') == cls) ++k;
        if (!inner) { while (k + 1 < e->len[y] && e_isspace(e->ln[y][k + 1])) ++k; }
        *y0 = y; *x0 = sx; *y1 = y; *x1 = k;
        return 1;
    }
    if (c == '"' || c == 39) {
        sx = x;
        while (sx >= 0 && e->ln[y][sx] != c) --sx;
        if (sx < 0) return 0;
        k = sx + 1;
        while (k < e->len[y] && e->ln[y][k] != c) ++k;
        if (k >= e->len[y]) return 0;
        if (inner) { *x0 = sx + 1; *x1 = k - 1; } else { *x0 = sx; *x1 = k; }
        *y0 = y; *y1 = y;
        return *x1 >= *x0;
    }
    if (c == '(' || c == ')' || c == 'b') { open = '('; close = ')'; }
    else if (c == '{' || c == '}' || c == 'B') { open = '{'; close = '}'; }
    else if (c == '[' || c == ']') { open = '['; close = ']'; }
    else return 0;
    // out to the enclosing OPEN, then on to its CLOSE
    sy = y; sx = x; depth = 0;
    while (1) {
        k = e_char(e, sy, sx);
        if (k == close && !(sy == y && sx == x)) ++depth;
        else if (k == open) { if (depth == 0) break; --depth; }
        if (!e_step(e, &sy, &sx, 0 - 1)) return 0;
    }
    *y0 = sy; *x0 = sx;
    if (!e_match(e, &sy, &sx)) return 0;
    *y1 = sy; *x1 = sx;
    if (inner) {
        if (!e_step(e, y0, x0, 1)) return 0;
        if (!e_step(e, y1, x1, 0 - 1)) return 0;
    }
    return 1;
}

// ---- the command line ----------------------------------------------------------------

// append S to D, as much as fits in a status line
static void k_copy_tail(char *d, char *s) {
    int n, k;
    n = ui_len(d);
    k = 0;
    while (s[k] && n < 110) { d[n++] = s[k++]; }
    d[n] = 0;
}

static void e_goto_error(struct win *w, int i) {
    struct edit *e;
    e = E(w);
    if (i < 0 || i >= e->en) { e_say(e, "No more errors"); return; }
    e->ecur = i;
    e->bsel = e->elog[i];
    if (e->bsel < e->btop) e->btop = e->bsel;
    if (e->bsel >= e->btop + BROWS) e->btop = e->bsel - BROWS + 1;
    if (!ui_eq(e->epath[i], e->path)) {
        if (e->modified && !e_save(e, e->path)) return;
        if (!e_load(e, e->epath[i])) { e_say(e, "Cannot open the file the error names"); return; }
        e_retitle(w);
    }
    e->cy = e->eline[i] - 1;
    e->cx = 0;
    e_clamp(e);
    e->cx = e_firstnb(e, e->cy);
    e->want = e->cx;
    ui_cpy(e->msg, "(");
    ui_num(e->msg + 1, i + 1);
    ui_cat(e->msg, " of ");
    ui_num(e->msg + ui_len(e->msg), e->en);
    ui_cat(e->msg, ") ");
    k_copy_tail(e->msg, e->blog[e->elog[i]]);
}

// :s/old/new/ and :%s/old/new/g, literal text
static void e_subst(struct edit *e, char *a, int all) {
    char pat[64], rep[64];
    int n, k, y, y0, y1, x, global, pn, rn, count;
    if (*a != '/') { e_say(e, "Usage: s/old/new/[g]"); return; }
    ++a;
    n = 0; while (*a && *a != '/' && n < 63) pat[n++] = *a++;
    pat[n] = 0; pn = n;
    if (*a == '/') ++a;
    n = 0; while (*a && *a != '/' && n < 63) rep[n++] = *a++;
    rep[n] = 0; rn = n;
    if (*a == '/') ++a;
    global = *a == 'g';
    if (!pn) { e_say(e, "Empty pattern"); return; }
    e_save_undo(e);
    y0 = all ? 0 : e->cy; y1 = all ? e->nl - 1 : e->cy;
    count = 0;
    y = y0;
    while (y <= y1) {
        x = 0;
        while (x + pn <= e->len[y]) {
            k = 0;
            while (k < pn && e->ln[y][x + k] == pat[k]) ++k;
            if (k == pn) {
                e_delrange(e, y, x, y, x + pn);
                if (rn) e_instext(e, y, x, rep, rn);
                x = x + rn;
                ++count;
                if (!global) break;
            } else ++x;
        }
        ++y;
    }
    ui_num(e->msg, count);
    ui_cat(e->msg, count == 1 ? " substitution" : " substitutions");
}

static void e_ex(struct win *w, char *c) {
    struct edit *e;
    int v, n;
    e = E(w);
    while (*c == ' ') ++c;
    if (!*c) return;
    if ((n = e_digits(c, &v)) > 0 && !c[n]) { e->cy = v - 1; e_clamp(e); e->cx = e_firstnb(e, e->cy); return; }
    if (ui_eq(c, "$")) { e->cy = e->nl - 1; e->cx = e_firstnb(e, e->cy); return; }
    if (ui_eq(c, "w")) { e_save(e, e->path); e_retitle(w); return; }
    if (c[0] == 'w' && c[1] == ' ') { e_save(e, c + 2); e_retitle(w); return; }
    if (ui_eq(c, "q")) {
        if (e->modified) { e_say(e, "No write since last change (add ! to override)"); return; }
        win_close(win_index(w)); return;
    }
    if (ui_eq(c, "q!")) { win_close(win_index(w)); return; }
    if (ui_eq(c, "wq") || ui_eq(c, "x")) { if (e_save(e, e->path)) win_close(win_index(w)); return; }
    if (c[0] == 'e' && (c[1] == ' ' || (c[1] == '!' && c[2] == ' '))) {
        if (c[1] != '!' && e->modified) { e_say(e, "No write since last change (add ! to override)"); return; }
        c = c + (c[1] == '!' ? 3 : 2);
        if (!e_open(w, c, 0)) { e_settext(e, "", 0); ui_ncpy(e->path, c, 128); e->modified = 0; e->cy = 0; e->cx = 0; e_say(e, "New file"); }
        e_retitle(w);
        return;
    }
    if (ui_eq(c, "make")) { e_make(w); return; }
    if (ui_eq(c, "Ex") || ui_eq(c, "Explore") || ui_eq(c, "Lex") || ui_eq(c, "files")) { e->fshown = !e->fshown; return; }
    if (ui_eq(c, "syntax off") || ui_eq(c, "syn off")) { e->nosyn = 1; return; }
    if (ui_eq(c, "syntax on") || ui_eq(c, "syn on")) { e->nosyn = 0; return; }
    if (ui_eq(c, "set wrap")) { e->wrap = 1; e->left = 0; return; }
    if (ui_eq(c, "set nowrap")) { e->wrap = 0; return; }
    if (ui_eq(c, "cancel")) { e_cancel(w); return; }
    if (ui_eq(c, "run") || (c[0] == 'r' && c[1] == 'u' && c[2] == 'n' && c[3] == ' ')) {
        if (c[3] == ' ') ui_cpy(e->runcmd, c + 4);
        if (!e->runcmd[0]) { e_say(e, "Nothing to run yet: :make first, or :run COMMAND"); return; }
        run_command(e->runcmd);
        return;
    }
    if (ui_eq(c, "cn") || ui_eq(c, "cnext")) { e_goto_error(w, e->ecur + 1); return; }
    if (ui_eq(c, "cp") || ui_eq(c, "cprev")) { e_goto_error(w, e->ecur - 1); return; }
    if (ui_eq(c, "copen")) { e->bshown = 1; return; }
    if (ui_eq(c, "cclose")) { e->bshown = 0; return; }
    if (ui_eq(c, "set nu") || ui_eq(c, "set number")) { e->nu = 1; return; }
    if (ui_eq(c, "set nonu") || ui_eq(c, "set nonumber")) { e->nu = 0; return; }
    if (ui_eq(c, "set vim")) { e->vim = 1; e->tsel = 0; e->mode = M_NORMAL; e_clamp(e); return; }
    if (ui_eq(c, "set novim")) { e->vim = 0; e->mode = M_INSERT; e_say(e, "VIM mode off: type to edit; Esc for a command (set vim to come back)"); return; }
    if (c[0] == 's' && c[1] == '/') { e_subst(e, c + 1, 0); return; }
    if (c[0] == '%' && c[1] == 's' && c[2] == '/') { e_subst(e, c + 2, 1); return; }
    ui_cpy(e->msg, "Not an editor command: ");
    k_copy_tail(e->msg, c);
}

static void e_cmdline_key(struct win *w, int code, int ch) {
    struct edit *e;
    int y, x;
    e = E(w);
    if (code == KEY_ESC) { e->mode = M_NORMAL; e->cmdn = 0; return; }
    if (code == KEY_BACK) {
        if (e->cmdn == 0) { e->mode = M_NORMAL; return; }
        --e->cmdn; e->cmd[e->cmdn] = 0; return;
    }
    if (code == KEY_ENTER) {
        e->cmd[e->cmdn] = 0;
        e->mode = M_NORMAL;
        if (e->cmdkind == ':') e_ex(w, e->cmd);
        else if (e->cmdn > 0) {
            ui_cpy(e->search, e->cmd);
            e->sdir = e->cmdkind == '/' ? 1 : 0 - 1;
            y = e->cy; x = e->cx;
            if (e_find(e, e->search, e->sdir, &y, &x)) { e->cy = y; e->cx = x; e->want = x; }
            else e_say(e, "Pattern not found");
        }
        e->cmdn = 0;
        return;
    }
    if (ch >= 32 && ch < 127 && e->cmdn < 150) { e->cmd[e->cmdn++] = ch; e->cmd[e->cmdn] = 0; }
}

// ---- keys --------------------------------------------------------------------------------

// Arrows and the rest, in Insert (and with VIM mode off). 1 if handled.
static int e_nav_insert(struct edit *e, int code, int mods) {
    int ty, tx, lw, incl;
    if (!(code == KEY_LEFT || code == KEY_RIGHT || code == KEY_UP || code == KEY_DOWN || code == KEY_HOME ||
          code == KEY_END || code == KEY_PGUP || code == KEY_PGDN)) return 0;
    if (code == KEY_RIGHT && !(mods & M_CTRL)) { if (e->cx < e->len[e->cy]) ++e->cx; else if (e->cy + 1 < e->nl) { ++e->cy; e->cx = 0; } e->want = e->cx; return 1; }
    if (code == KEY_LEFT && !(mods & M_CTRL)) { if (e->cx > 0) --e->cx; else if (e->cy > 0) { --e->cy; e->cx = e->len[e->cy]; } e->want = e->cx; return 1; }
    if (code == KEY_END && !(mods & M_CTRL)) { e->cx = e->len[e->cy]; e->want = 100000; return 1; }
    if (!e_motion(e, code, 0, mods, 1, &ty, &tx, &lw, &incl)) return 0;
    e->cy = ty; e->cx = tx;
    if (e->cy < 0) e->cy = 0;
    if (e->cy >= e->nl) e->cy = e->nl - 1;
    if (e->cx > e->len[e->cy]) e->cx = e->len[e->cy];
    if (!lw) e->want = e->cx;
    return 1;
}

static void e_insert_key(struct edit *e, int code, int ch, int mods) {
    if (e_nav_insert(e, code, mods)) return;
    if (code == KEY_ESC) {
        if (!e->vim) return;
        e->mode = M_NORMAL;
        if (e->cx > 0) --e->cx;
        e_clamp(e);
        return;
    }
    if (code == KEY_ENTER) { e_newline(e); e->want = e->cx; return; }
    if (code == KEY_BACK) {
        if (e->cx > 0) { e_delrange(e, e->cy, e->cx - 1, e->cy, e->cx); --e->cx; }
        else if (e->cy > 0) { e->cx = e->len[e->cy - 1]; --e->cy; e_delrange(e, e->cy, e->cx, e->cy + 1, 0); }
        e->want = e->cx;
        return;
    }
    if (code == KEY_DEL) {
        if (e->cx < e->len[e->cy]) e_delrange(e, e->cy, e->cx, e->cy, e->cx + 1);
        else if (e->cy + 1 < e->nl) e_delrange(e, e->cy, e->cx, e->cy + 1, 0);
        return;
    }
    if (code == KEY_TAB) { e_instext(e, e->cy, e->cx, "    ", 4); e->cx = e->cx + 4; e->want = e->cx; return; }
    if (mods & M_CTRL) return;
    if (ch >= 32 && ch < 127) { e_inschar(e, ch); e->want = e->cx; }
}

static void e_visual_key(struct win *w, int code, int ch, int mods) {
    struct edit *e;
    int ty, tx, lw, incl, r, n, t;
    e = E(w);
    if (code == KEY_ESC || (ch == 'v' && e->mode == M_VISUAL) || (ch == 'V' && e->mode == M_VLINE)) { e->mode = M_NORMAL; e_clamp(e); return; }
    if (ch == 'v') { e->mode = M_VISUAL; return; }
    if (ch == 'V') { e->mode = M_VLINE; return; }
    if (ch == 'o') { t = e->cy; e->cy = e->vy; e->vy = t; t = e->cx; e->cx = e->vx; e->vx = t; return; }
    if (ch == 'd' || ch == 'x' || ch == 'y' || ch == 'c' || ch == '>' || ch == '<' || ch == 'D' || ch == 'X' || ch == 'Y') {
        r = ch == 'x' ? 'd' : (ch == 'D' || ch == 'X') ? 'd' : ch == 'Y' ? 'y' : ch;
        e_apply(e, r, e->vy, e->vx, e->cy, e->cx, e->mode == M_VLINE || ch == 'D' || ch == 'X' || ch == 'Y' || ch == '>' || ch == '<', 1);
        if (e->mode != M_INSERT) e->mode = M_NORMAL;
        e_clamp(e);
        return;
    }
    if (ch == 'J') { e->cy = e->vy < e->cy ? e->vy : e->cy; e_join(e, (e->vy > e->cy ? e->vy - e->cy : 1)); e->mode = M_NORMAL; return; }
    if (ch >= '1' && ch <= '9') { e->count = e->count * 10 + (ch - '0'); return; }
    n = e->count ? e->count : 1;
    r = e_motion(e, code, ch, mods & M_CTRL, n, &ty, &tx, &lw, &incl);
    e->count = 0;
    if (r == 1) { e->cy = ty; e->cx = tx; e_clamp(e); if (!lw) e->want = e->cx; }
}

// What a Normal-mode command does once its keys are all in.
static void e_normal_key(struct win *w, int code, int ch, int mods) {
    struct edit *e;
    int n, r, ty, tx, lw, incl, y0, x0, y1, x1, op;
    e = E(w);

    // the second key of a two-key command
    if (e->pre) {
        r = e->pre; e->pre = 0;
        n = e->count ? e->count : 1;
        if (r == 'g') {
            if (ch != 'g') { e->count = 0; e->op = 0; return; }
            ty = e->count ? e->count - 1 : 0;
            e->count = 0;
            if (ty >= e->nl) ty = e->nl - 1;
            if (e->op) { op = e->op; e->op = 0; e_apply(e, op, e->cy, 0, ty, 0, 1, 0); e_clamp(e); return; }
            e->cy = ty; e_clamp(e); e->cx = e_firstnb(e, e->cy);
            return;
        }
        if (r == 'r') {
            e->count = 0;
            if (ch < 32 || ch >= 127 || e->len[e->cy] == 0) return;
            e_save_undo(e);
            while (n > 0 && e->cx < e->len[e->cy]) { e->ln[e->cy][e->cx] = ch; ++e->cx; --n; }
            --e->cx;
            e->modified = 1; e->changed = 1; ++e->hver;
            return;
        }
        if (r == 'i' || r == 'a') {
            op = e->op; e->op = 0; e->count = 0;
            if (!e_textobj(e, r == 'i', ch, &y0, &x0, &y1, &x1)) return;
            e_apply(e, op, y0, x0, y1, x1, 0, 1);
            e_clamp(e);
            return;
        }
        if (r == 'Z') { e->count = 0; if (ch == 'Z') e_ex(w, "x"); else if (ch == 'Q') e_ex(w, "q!"); return; }
        // f t F T
        if (ch < 32 || ch >= 127) { e->count = 0; e->op = 0; return; }
        e->fkind = r; e->fch = ch;
        if (!e_motion_find(e, r, ch, n, &ty, &tx, &incl)) { e->count = 0; e->op = 0; return; }
        lw = 0;
        e->count = 0;
        if (e->op) { e_apply(e, e->op, e->cy, e->cx, ty, tx, 0, incl); e->op = 0; e_clamp(e); return; }
        e->cx = tx; e->want = tx;
        return;
    }

    if ((ch >= '1' && ch <= '9') || (ch == '0' && e->count > 0)) { e->count = e->count * 10 + (ch - '0'); return; }
    n = e->count ? e->count : 1;

    if (e->op) {
        op = e->op;
        // dd cc yy >> <<: lines
        if (ch == op) {
            y1 = e->cy + n - 1;
            if (y1 >= e->nl) y1 = e->nl - 1;
            e->op = 0; e->count = 0;
            e_apply(e, op, e->cy, 0, y1, 0, 1, 0);
            e_clamp(e);
            return;
        }
        if (ch == 'i' || ch == 'a') { e->pre = ch; return; }
        r = e_motion(e, code, ch, mods, n, &ty, &tx, &lw, &incl);
        if (r < 0) return;                      // f/t/g: wait for the next key
        e->op = 0; e->count = 0;
        if (r == 0 || code == KEY_ESC) return;
        if (ty < 0) ty = 0;
        if (ty >= e->nl) ty = e->nl - 1;
        // cw is ce; and dw on a line's last word stops at its end
        if ((ch == 'w' || ch == 'W') && op == 'c') {
            ty = e->cy; tx = e->cx; r = n;
            if (e_class(e_char(e, ty, tx), ch == 'W') != 0) { while (r--) e_word_end(e, &ty, &tx, ch == 'W'); incl = 1; }
        } else if ((ch == 'w' || ch == 'W') && ty > e->cy) { ty = e->cy; tx = e->len[e->cy]; }
        e_apply(e, op, e->cy, e->cx, ty, tx, lw, incl);
        e_clamp(e);
        return;
    }

    r = e_motion(e, code, ch, mods, n, &ty, &tx, &lw, &incl);
    if (r < 0) return;
    if (r > 0) {
        e->count = 0;
        // Shift+arrows select
        if ((mods & M_SHIFT) && (code == KEY_UP || code == KEY_DOWN || code == KEY_LEFT || code == KEY_RIGHT)) {
            e->vy = e->cy; e->vx = e->cx;
            e->mode = (code == KEY_UP || code == KEY_DOWN) ? M_VLINE : M_VISUAL;
        }
        e->cy = ty; e->cx = tx;
        e_clamp(e);
        if (!lw) e->want = e->cx;
        if (ch == 'G' || (mods & M_CTRL && (code == KEY_HOME || code == KEY_END))) e->cx = e_firstnb(e, e->cy);
        return;
    }
    e->count = 0;
    if (code == KEY_ESC) { e->op = 0; return; }
    if (code == KEY_DEL) ch = 'x';
    if (mods & M_CTRL) {
        if (code == 82) e_undo(e, 1);              // Ctrl-R
        return;
    }
    if (ch == 'd' || ch == 'c' || ch == 'y' || ch == '>' || ch == '<') { e->op = ch; e->count = n > 1 ? n : 0; return; }
    if (ch == 'r' || ch == 'Z') { e->pre = ch; e->count = n > 1 ? n : 0; return; }
    if (ch == 'i') { e_save_undo(e); e->mode = M_INSERT; return; }
    if (ch == 'a') { e_save_undo(e); e->mode = M_INSERT; if (e->len[e->cy] > 0) ++e->cx; return; }
    if (ch == 'I') { e_save_undo(e); e->mode = M_INSERT; e->cx = e_firstnb(e, e->cy); return; }
    if (ch == 'A') { e_save_undo(e); e->mode = M_INSERT; e->cx = e->len[e->cy]; return; }
    if (ch == 'o' || ch == 'O') { e_open_line(e, ch == 'O'); return; }
    if (ch == 'x' || ch == 'X') {
        if (e->len[e->cy] == 0) return;
        if (ch == 'X' && e->cx == 0) return;
        if (ch == 'X') { e_apply(e, 'd', e->cy, e->cx - n < 0 ? 0 : e->cx - n, e->cy, e->cx, 0, 0); return; }
        x1 = e->cx + n; if (x1 > e->len[e->cy]) x1 = e->len[e->cy];
        e_apply(e, 'd', e->cy, e->cx, e->cy, x1, 0, 0);
        e_clamp(e);
        return;
    }
    if (ch == 'D' || ch == 'C') { e_apply(e, ch == 'D' ? 'd' : 'c', e->cy, e->cx, e->cy, e->len[e->cy], 0, 0); e_clamp(e); return; }
    if (ch == 's') { x1 = e->cx + n; if (x1 > e->len[e->cy]) x1 = e->len[e->cy]; e_apply(e, 'c', e->cy, e->cx, e->cy, x1, 0, 0); return; }
    if (ch == 'S') { y1 = e->cy + n - 1; if (y1 >= e->nl) y1 = e->nl - 1; e_apply(e, 'c', e->cy, 0, y1, 0, 1, 0); return; }
    if (ch == 'Y') { y1 = e->cy + n - 1; if (y1 >= e->nl) y1 = e->nl - 1; e_yank(e, e->cy, 0, y1, 0, 1); return; }
    if (ch == 'p' || ch == 'P') { while (n--) e_put(e, ch == 'P'); return; }
    if (ch == 'J') { e_join(e, n > 1 ? n - 1 : 1); return; }
    if (ch == '~') {
        if (e->len[e->cy] == 0) return;
        e_save_undo(e);
        while (n-- > 0 && e->cx < e->len[e->cy]) {
            r = e->ln[e->cy][e->cx];
            if (r >= 'a' && r <= 'z') r = r - 32; else if (r >= 'A' && r <= 'Z') r = r + 32;
            e->ln[e->cy][e->cx] = r; ++e->cx;
        }
        e->modified = 1; e->changed = 1; ++e->hver;
        e_clamp(e);
        return;
    }
    if (ch == 'u') { e_undo(e, 0); e_clamp(e); return; }
    if (ch == 'v' || ch == 'V') { e->vy = e->cy; e->vx = e->cx; e->mode = ch == 'v' ? M_VISUAL : M_VLINE; return; }
    if (ch == ':' || ch == '/' || ch == '?') { e->mode = M_CMD; e->cmdkind = ch; e->cmdn = 0; e->cmd[0] = 0; return; }
    if (ch == '*') {
        // the word under the cursor, searched forward
        if (!e_isword(e_char(e, e->cy, e->cx))) return;
        x0 = e->cx; while (x0 > 0 && e_isword(e->ln[e->cy][x0 - 1])) --x0;
        x1 = e->cx; while (x1 < e->len[e->cy] && e_isword(e->ln[e->cy][x1])) ++x1;
        if (x1 - x0 > 62) x1 = x0 + 62;
        memcpy(e->search, e->ln[e->cy] + x0, x1 - x0);
        e->search[x1 - x0] = 0;
        e->sdir = 1;
        ty = e->cy; tx = e->cx;
        if (e_find(e, e->search, 1, &ty, &tx)) { e->cy = ty; e->cx = tx; e->want = tx; }
        return;
    }
}

// ---- the selection and the clipboard -----------------------------------------------------
//
// A selection is VIM's Visual one, or Traditional's (tsel), from (vy, vx)
// to the cursor. As a range it runs up to, not including, (y1, x1): a
// Visual one takes the character under the cursor too, and a line-wise one
// whole lines with their newlines.

static int e_sel_on(struct edit *e) { return e->vim ? e->mode == M_VISUAL || e->mode == M_VLINE : e->tsel; }

static int e_sel_range(struct edit *e, int *y0, int *x0, int *y1, int *x1) {
    int t;
    if (!e_sel_on(e)) return 0;
    *y0 = e->vy; *x0 = e->vx; *y1 = e->cy; *x1 = e->cx;
    if (*y1 < *y0 || (*y1 == *y0 && *x1 < *x0)) { t = *y0; *y0 = *y1; *y1 = t; t = *x0; *x0 = *x1; *x1 = t; }
    if (*y0 < 0) *y0 = 0;
    if (*y1 >= e->nl) { *y1 = e->nl - 1; *x1 = e->len[*y1]; }
    if (e->vim && e->mode == M_VLINE) { *x0 = 0; *x1 = e->len[*y1] + 1; }
    else if (e->vim) *x1 = *x1 + 1;
    return *y1 > *y0 || *x1 > *x0;
}

// the selection gone: VIM back to Normal, Traditional to none
static void e_sel_end(struct edit *e) {
    e->tsel = 0;
    if (e->vim && (e->mode == M_VISUAL || e->mode == M_VLINE)) { e->mode = M_NORMAL; e_clamp(e); }
}

static void e_copy(struct edit *e) {
    char *t;
    int n, y0, x0, y1, x1;
    if (!e_sel_range(e, &y0, &x0, &y1, &x1)) return;
    t = e_range(e, y0, x0, y1, x1, &n);
    wk_clip_set(t, n);
    free(t);
}

// the selection taken out of the text, the cursor where it began: 1 if there was one
static int e_cut(struct edit *e) {
    int y0, x0, y1, x1;
    if (!e_sel_range(e, &y0, &x0, &y1, &x1)) { e_sel_end(e); return 0; }
    e_delrange(e, y0, x0, y1, x1);
    e->cy = y0; e->cx = x0; e->want = x0;
    e_sel_end(e);
    e_clamp(e);
    return 1;
}

// the clipboard's text at the cursor, in place of the selection (its
// carriage returns left out: a line ends in a newline here)
static void e_paste(struct edit *e) {
    char *c, *t;
    int n, k, m;
    c = wk_clip_get(&n);
    if (n <= 0) return;
    e_save_undo(e);
    e_cut(e);
    t = (char *)ualloc(n + 1);
    m = 0; k = 0;
    while (k < n) { if (c[k] != 13 && c[k]) { t[m] = c[k]; ++m; } ++k; }
    e_instext(e, e->cy, e->cx, t, m);
    free(t);
    e->cy = e_ry; e->cx = e_rx; e->want = e->cx;
    e_clamp(e);
}

static void e_edit(struct win *w, int item) {
    struct edit *e;
    e = E(w);
    if (e->mode == M_CMD) e->mode = e->vim ? M_NORMAL : M_INSERT;
    if (item == E_COPY) e_copy(e);
    else if (item == E_CUT) {
        if (!e_sel_on(e)) return;
        e_save_undo(e);
        e_copy(e);
        e_cut(e);
    }
    else if (item == E_PASTE) e_paste(e);
    else if (item == E_ALL) {
        e->vy = 0; e->vx = 0;
        e->cy = e->nl - 1; e->cx = e->len[e->cy];
        if (e->vim) { e->mode = M_VISUAL; if (e->cx > 0) --e->cx; }
        else e->tsel = e->nl > 1 || e->cx > 0;
    }
    e->tlast = 0;
    e_retitle(w);
}

static int e_ctrl(int code, int ch, int mods, int c) { return (mods & M_CTRL) && (code == c || ch == c || ch == c + 32); }

// Traditional: Text Editor's keys. The moving keys with Shift select; the
// typing keys replace a selection; a run of characters typed is undone
// as one.
static void e_trad_key(struct win *w, int code, int ch, int mods) {
    struct edit *e;
    int typing, y0, x0, y1, x1;
    e = E(w);
    if (code == 16 || code == 17 || code == 18) return;       // Shift, Ctrl or Alt alone
    if (ui_key_copy(code, ch, mods)) { e_edit(w, E_COPY); return; }
    if (ui_key_cut(code, ch, mods)) { e_edit(w, E_CUT); return; }
    if (ui_key_paste(code, ch, mods)) { e_edit(w, E_PASTE); return; }
    if (e_ctrl(code, ch, mods, 'A')) { e_edit(w, E_ALL); return; }
    if (e_ctrl(code, ch, mods, 'Z')) { e_undo(e, 0); e->tsel = 0; e->tlast = 0; e_clamp(e); return; }
    if (e_ctrl(code, ch, mods, 'Y')) { e_undo(e, 1); e->tsel = 0; e->tlast = 0; e_clamp(e); return; }
    if (e_ctrl(code, ch, mods, 'O')) { edit_command(w, 0, 1); return; }
    if (e_ctrl(code, ch, mods, 'N')) { edit_command(w, 0, 0); return; }
    if (code == KEY_ESC) {
        if (e->tsel) { e->tsel = 0; return; }
        e->mode = M_CMD; e->cmdkind = ':'; e->cmdn = 0; e->cmd[0] = 0;
        return;
    }
    if (code == KEY_LEFT || code == KEY_RIGHT || code == KEY_UP || code == KEY_DOWN || code == KEY_HOME ||
        code == KEY_END || code == KEY_PGUP || code == KEY_PGDN) {
        e->tlast = 0;
        if (mods & M_SHIFT) {
            if (!e->tsel) { e->vy = e->cy; e->vx = e->cx; }
            e_nav_insert(e, code, mods);
            e->tsel = e->vy != e->cy || e->vx != e->cx;
            return;
        }
        // Left and Right with a selection go to its ends
        if (e->tsel && !(mods & M_CTRL) && (code == KEY_LEFT || code == KEY_RIGHT) && e_sel_range(e, &y0, &x0, &y1, &x1)) {
            if (code == KEY_LEFT) { e->cy = y0; e->cx = x0; } else { e->cy = y1; e->cx = x1; }
            e->want = e->cx; e->tsel = 0;
            return;
        }
        e->tsel = 0;
        e_nav_insert(e, code, mods);
        return;
    }
    typing = ch >= 32 && ch < 127 && !(mods & M_CTRL);
    if (!typing && code != KEY_ENTER && code != KEY_BACK && code != KEY_DEL && code != KEY_TAB) return;
    if (!typing || !e->tlast || e->tsel) e_save_undo(e);
    e->tlast = typing;
    if (e->tsel) {
        e_cut(e);
        if (code == KEY_BACK || code == KEY_DEL) return;
    }
    e_insert_key(e, code, ch, mods);
}

// The way in for every key. It records the keys of a change as it is
// made, so '.' can type them again.
static void e_key(struct win *w, int code, int ch, int mods) {
    struct edit *e;
    int was_normal, k;
    e = E(w);
    e->msg[0] = 0;
    // Ctrl+S saves, whatever the mode
    if ((mods & M_CTRL) && (code == 83 || ch == 's' || ch == 'S')) { e_save(e, e->path); e_retitle(w); return; }
    // Ctrl+B: the file pane, as IDEs have it (VIM's page back is not here)
    if ((mods & M_CTRL) && (code == 66 || ch == 'b' || ch == 'B')) { e->fshown = !e->fshown; return; }
    if (code == KEY_F5) { e_make(w); return; }
    if (code == KEY_F6) { e_ex(w, "run"); return; }
    if (code == KEY_F8) { e_goto_error(w, (mods & M_SHIFT) ? e->ecur - 1 : e->ecur + 1); return; }
    if (code == KEY_F4) { e->bshown = !e->bshown; return; }
    if (e->mode == M_CMD) {
        e_cmdline_key(w, code, ch);
        // without VIM mode there is no Normal to come back to
        if (!e->vim && e->mode == M_NORMAL) e->mode = M_INSERT;
        return;
    }
    if (!e->vim) {
        // Traditional: always typing; Esc is the way to a command
        e_trad_key(w, code, ch, mods);
        e_retitle(w);
        return;
    }

    // '.': the last change, again
    if (e->mode == M_NORMAL && !e->op && !e->pre && ch == '.' && !(mods & M_CTRL)) {
        if (!e->dotn) return;
        e->replaying = 1;
        k = 0;
        while (k < e->dotn) { e_key(w, e->dot[k * 3], e->dot[k * 3 + 1], e->dot[k * 3 + 2]); k = k + 1; }
        e->replaying = 0;
        return;
    }
    was_normal = e->mode == M_NORMAL && !e->op && !e->pre && !e->count;
    if (was_normal && !e->replaying) { e->recn = 0; e->recording = 1; e->changed = 0; }
    if (e->recording && !e->replaying && e->recn < REC_MAX) {
        e->rec[e->recn * 3] = code; e->rec[e->recn * 3 + 1] = ch; e->rec[e->recn * 3 + 2] = mods;
        ++e->recn;
    }

    if (e->mode == M_INSERT) e_insert_key(e, code, ch, mods);
    else if (e->mode == M_VISUAL || e->mode == M_VLINE) e_visual_key(w, code, ch, mods);
    else e_normal_key(w, code, ch, mods);

    // a change is done when Normal mode has nothing half typed
    if (e->recording && e->mode == M_NORMAL && !e->op && !e->pre && !e->count) {
        if (e->changed && !e->replaying && e->recn) {
            memcpy(e->dot, e->rec, e->recn * 3 * sizeof(int));
            e->dotn = e->recn;
        }
        e->recording = 0;
    }
    e_retitle(w);
}

void edit_key(struct win *w, int code, int ch, int mods) { e_key(w, code, ch, mods); }

// ---- syntax colours ---------------------------------------------------------------------
//
// Each line is coloured on its own, character by character, into hlc; the
// one thing carried from line to line is being inside a C comment. Where
// that stands at the top of the view is kept, with the count of changes
// it was worked out for, so scrolling down does not read the file again.

static int hlc[HL_MAX];

static int e_lang(char *p) {
    if (ui_ends(p, ".c") || ui_ends(p, ".h")) return L_C;
    if (ui_ends(p, ".js") || ui_ends(p, ".mjs")) return L_JS;
    if (ui_ends(p, ".lisp") || ui_ends(p, ".scm") || ui_ends(p, ".el")) return L_LISP;
    if (ui_ends(p, ".f") || ui_ends(p, ".fs") || ui_ends(p, ".fth") || ui_ends(p, ".4th")) return L_FORTH;
    if (ui_ends(p, ".sh") || ui_ends(p, "Makefile") || ui_ends(p, ".mk")) return L_SH;
    if (ui_ends(p, ".b4k") || ui_ends(p, ".B4K")) return L_B4K;
    return L_TEXT;
}

// Is S[0..N) one of LIST's words? LIST has a space before each word and
// after the last.
static int e_inwords(char *s, int n, char *list) {
    int i, k;
    i = 0;
    while (list[i]) {
        if (list[i] == ' ' && list[i + 1]) {
            k = 0;
            while (k < n && list[i + 1 + k] == s[k]) ++k;
            if (k == n && list[i + 1 + k] == ' ') return 1;
        }
        ++i;
    }
    return 0;
}

static void e_paint(int i, int j, int c) {
    while (i < j && i < HL_MAX) { hlc[i] = c; ++i; }
}
static int e_hcol(int i) { return ec[i < HL_MAX ? hlc[i] : HC_TEXT]; }

static char *c_kw = " if else while for do switch case default break continue return goto sizeof typedef struct union enum static extern const volatile register inline restrict ";
static char *c_ty = " int char short long unsigned signed void float double ";
static char *js_kw = " function var let const if else for while do return new class this null true false undefined import export from of in typeof instanceof await async break continue switch case default throw try catch finally delete yield ";
static char *lisp_kw = " define lambda if begin let let* letrec set! cond else and or not quote next define-macro when unless do case ";
static char *forth_kw = " IF ELSE THEN BEGIN WHILE REPEAT UNTIL AGAIN DO ?DO LOOP +LOOP LEAVE UNLOOP EXIT ; : VARIABLE CONSTANT VALUE CREATE DOES> RECURSE DEFER IS TO CASE OF ENDOF ENDCASE IMMEDIATE POSTPONE ['] [CHAR] LITERAL :NONAME {: :} :M ;M ";
static char *sh_kw = " if then else elif fi for in do done while until case esac function return export local set exit break continue shift ";

// a string from I (its quote) to its end, \ escapes skipped
static int e_strend(char *s, int n, int i) {
    int q;
    q = s[i];
    ++i;
    while (i < n && s[i] != q) { if (s[i] == 92) ++i; ++i; }
    return i < n ? i + 1 : n;
}

// C and JavaScript; ST 1 is inside /* */
static int e_hl_c(char *s, int n, int st, int js) {
    int i, j, c, nb;
    i = 0; nb = 0;
    while (i < n) {
        c = s[i];
        if (st == 1) {
            j = i;
            while (j < n && !(s[j] == '*' && j + 1 < n && s[j + 1] == '/')) ++j;
            if (j < n) { j = j + 2; st = 0; }
            e_paint(i, j, HC_COM); i = j;
        } else if (c == '/' && i + 1 < n && s[i + 1] == '/') { e_paint(i, n, HC_COM); i = n; }
        else if (c == '/' && i + 1 < n && s[i + 1] == '*') { e_paint(i, i + 2, HC_COM); i = i + 2; st = 1; }
        else if (c == '"' || c == 39 || (js && c == '`')) { j = e_strend(s, n, i); e_paint(i, j, HC_STR); i = j; }
        else if (!js && c == '#' && !nb) {
            j = i + 1;
            while (j < n && s[j] == ' ') ++j;
            while (j < n && e_isword(s[j])) ++j;
            e_paint(i, j, HC_PRE); i = j;
        } else if (c >= '0' && c <= '9' && (i == 0 || !e_isword(s[i - 1]))) {
            j = i;
            while (j < n && (e_isword(s[j]) || s[j] == '.')) ++j;
            e_paint(i, j, HC_NUM); i = j;
        } else if (e_isword(c)) {
            j = i;
            while (j < n && e_isword(s[j])) ++j;
            if (e_inwords(s + i, j - i, js ? js_kw : c_kw)) e_paint(i, j, HC_KW);
            else if (!js && e_inwords(s + i, j - i, c_ty)) e_paint(i, j, HC_TYPE);
            i = j;
        } else ++i;
        if (c != ' ' && c != 9) nb = 1;
    }
    return st;
}

static int e_lispword(int c) { return c > ' ' && c != '(' && c != ')' && c != '"' && c != ';' && c != 39; }
static void e_hl_lisp(char *s, int n) {
    int i, j, c;
    i = 0;
    while (i < n) {
        c = s[i];
        if (c == ';') { e_paint(i, n, HC_COM); i = n; }
        else if (c == '"') { j = e_strend(s, n, i); e_paint(i, j, HC_STR); i = j; }
        else if (e_lispword(c)) {
            j = i;
            while (j < n && e_lispword(s[j])) ++j;
            if ((c >= '0' && c <= '9') || (c == '-' && j > i + 1 && s[i + 1] >= '0' && s[i + 1] <= '9') || c == '#')
                e_paint(i, j, HC_NUM);
            else if (e_inwords(s + i, j - i, lisp_kw)) e_paint(i, j, HC_KW);
            i = j;
        } else ++i;
    }
}

// Forth: blank-separated words, case not mattering; \ and ( comment, S" ." C"
// ABORT" take a string, and : the name it defines
static void e_hl_forth(char *s, int n) {
    int i, j, k, c, def;
    char up[24];
    i = 0; def = 0;
    while (i < n) {
        if (s[i] == ' ' || s[i] == 9) { ++i; }
        else {
            j = i;
            while (j < n && s[j] != ' ' && s[j] != 9) ++j;
            k = 0;
            while (k < j - i && k < 23) { c = s[i + k]; up[k] = c >= 'a' && c <= 'z' ? c - 32 : c; ++k; }
            if (j - i == 1 && s[i] == 92) { e_paint(i, n, HC_COM); j = n; }
            else if (j - i == 1 && s[i] == '(') {
                k = j; while (k < n && s[k] != ')') ++k;
                if (k < n) ++k;
                e_paint(i, k, HC_COM); j = k;
            } else if (j - i <= 23 && j > i && s[j - 1] == '"' && (j - i == 2 || (j - i == 6 && up[0] == 'A'))) {
                e_paint(i, j, HC_KW);
                k = j + 1; while (k < n && s[k] != '"') ++k;
                if (k < n) ++k;
                e_paint(j, k, HC_STR); j = k;
            } else if (def) { e_paint(i, j, HC_TYPE); def = 0; }
            else if ((s[i] >= '0' && s[i] <= '9') || (s[i] == '-' && j > i + 1 && s[i + 1] >= '0' && s[i + 1] <= '9'))
                e_paint(i, j, HC_NUM);
            else if (j - i <= 23 && e_inwords(up, j - i, forth_kw)) {
                e_paint(i, j, HC_KW);
                if (j - i == 1 && s[i] == ':') def = 1;
            }
            i = j;
        }
    }
}

// shell, Makefiles and B4KE: # comments, quotes, $variables; a B4KE line's
// first word, in capitals, is its directive
static void e_hl_sh(char *s, int n, int b4k) {
    int i, j, c, first;
    i = 0; first = 1;
    while (i < n) {
        c = s[i];
        if (c == '#' && (i == 0 || s[i - 1] == ' ' || s[i - 1] == 9)) { e_paint(i, n, HC_COM); i = n; }
        else if (c == '"' || c == 39) { j = e_strend(s, n, i); e_paint(i, j, HC_STR); i = j; first = 0; }
        else if (c == '$') {
            j = i + 1;
            if (j < n && (s[j] == '{' || s[j] == '(')) { c = s[j] == '{' ? '}' : ')'; while (j < n && s[j] != c) ++j; if (j < n) ++j; }
            else { while (j < n && e_isword(s[j])) ++j; }
            e_paint(i, j, HC_TYPE); i = j; first = 0;
        } else if (e_isword(c)) {
            j = i;
            while (j < n && e_isword(s[j])) ++j;
            if (b4k && first && c >= 'A' && c <= 'Z') e_paint(i, j, HC_KW);
            else if (!b4k && e_inwords(s + i, j - i, sh_kw)) e_paint(i, j, HC_KW);
            else if (c >= '0' && c <= '9') e_paint(i, j, HC_NUM);
            i = j; first = 0;
        } else { if (c != ' ' && c != 9) first = 0; ++i; }
    }
}

// Colour line Y into hlc; ST is the state at its start, and the state at
// its end comes back
static int e_hl(struct edit *e, int y, int st, int lang) {
    int n;
    n = e->len[y];
    e_paint(0, n, HC_TEXT);
    if (lang == L_C || lang == L_JS) return e_hl_c(e->ln[y], n, st, lang == L_JS);
    if (lang == L_LISP) e_hl_lisp(e->ln[y], n);
    else if (lang == L_FORTH) e_hl_forth(e->ln[y], n);
    else if (lang == L_SH || lang == L_B4K) e_hl_sh(e->ln[y], n, lang == L_B4K);
    return 0;
}

// the state at the start of line LINE
static int e_hlstate(struct edit *e, int line, int lang) {
    int y, st;
    if (lang != L_C && lang != L_JS) return 0;
    y = 0; st = 0;
    if (e->hl_ver == e->hver && e->hl_lang == lang && e->hl_line <= line) { y = e->hl_line; st = e->hl_state; }
    while (y < line && y < e->nl) { st = e_hl(e, y, st, lang); ++y; }
    e->hl_line = line; e->hl_state = st; e->hl_ver = e->hver; e->hl_lang = lang;
    return st;
}

// ---- the file pane ---------------------------------------------------------------------

static void f_reserve(struct edit *e, int need) {
    char **np;
    int *nd, *nf, m, i;
    if (need <= e->fcap) return;
    m = e->fcap ? e->fcap * 2 : 128;
    while (m < need) m = m * 2;
    np = (char **)ualloc(m * sizeof(int));
    nd = (int *)ualloc(m * sizeof(int));
    nf = (int *)ualloc(m * sizeof(int));
    i = 0;
    while (i < e->fn) { np[i] = e->fpath[i]; nd[i] = e->fdepth[i]; nf[i] = e->fflag[i]; ++i; }
    if (e->fpath) { free((char *)e->fpath); free((char *)e->fdepth); free((char *)e->fflag); }
    e->fpath = np; e->fdepth = nd; e->fflag = nf; e->fcap = m;
}

static void f_ins(struct edit *e, int at, char *path, int depth, int flag) {
    int i;
    f_reserve(e, e->fn + 1);
    i = e->fn;
    while (i > at) { e->fpath[i] = e->fpath[i - 1]; e->fdepth[i] = e->fdepth[i - 1]; e->fflag[i] = e->fflag[i - 1]; --i; }
    e->fpath[at] = (char *)ualloc(ui_len(path) + 1);
    ui_cpy(e->fpath[at], path);
    e->fdepth[at] = depth;
    e->fflag[at] = flag;
    ++e->fn;
}

static void f_del(struct edit *e, int at) {
    int i;
    free(e->fpath[at]);
    i = at;
    while (i < e->fn - 1) { e->fpath[i] = e->fpath[i + 1]; e->fdepth[i] = e->fdepth[i + 1]; e->fflag[i] = e->fflag[i + 1]; ++i; }
    --e->fn;
}

static void f_join(char *out, char *dir, char *nm) {
    ui_cpy(out, dir);
    if (!(dir[0] == '/' && dir[1] == 0)) ui_cat(out, "/");
    ui_cat(out, nm);
}
static int f_before(char *a, char *b) {
    while (*a && *a == *b) { ++a; ++b; }
    return (*a & 255) < (*b & 255);
}

// Folder I's contents below it -- I -1 is the root, whose contents are
// the top of the tree -- folders first, each lot by name.
static void f_open_dir(struct edit *e, int i) {
    char *dir, *names, nm[FNAME];
    int *dirs, depth, n, k, j, r;
    dir = i < 0 ? "/" : e->fpath[i];
    depth = i < 0 ? 0 : e->fdepth[i] + 1;
    names = (char *)ualloc(F_KIDS * FNAME);
    dirs = (int *)ualloc(F_KIDS * sizeof(int));
    n = 0; k = 0;
    while (n < F_KIDS) {
        r = ureaddir(dir, k, nm);
        if (r < 0) break;
        ui_cpy(names + n * FNAME, nm);
        dirs[n] = r == 1;
        ++n; ++k;
    }
    k = 1;
    while (k < n) {
        j = k;
        while (j > 0 && (dirs[j] > dirs[j - 1] || (dirs[j] == dirs[j - 1] && f_before(names + j * FNAME, names + (j - 1) * FNAME)))) {
            ui_cpy(nm, names + j * FNAME); ui_cpy(names + j * FNAME, names + (j - 1) * FNAME); ui_cpy(names + (j - 1) * FNAME, nm);
            r = dirs[j]; dirs[j] = dirs[j - 1]; dirs[j - 1] = r;
            --j;
        }
        ++k;
    }
    j = 0;
    while (j < n) {
        f_join(sb, dir, names + j * FNAME);
        f_ins(e, i + 1 + j, sb, depth, dirs[j] ? F_DIR : 0);
        ++j;
    }
    if (i >= 0) e->fflag[i] = e->fflag[i] | F_OPEN;
    free(names); free((char *)dirs);
}

static void f_close_dir(struct edit *e, int i) {
    while (i + 1 < e->fn && e->fdepth[i + 1] > e->fdepth[i]) f_del(e, i + 1);
    e->fflag[i] = e->fflag[i] & F_DIR;
}

static int f_find(struct edit *e, char *p) {
    int i;
    i = 0;
    while (i < e->fn) { if (ui_eq(e->fpath[i], p)) return i; ++i; }
    return 0 - 1;
}

// open every folder above PATH, so that it is in the tree to be seen
static void f_reveal(struct edit *e, char *path) {
    char part[128];
    int k, i;
    if (!e->fpath || path[0] != '/') return;
    k = 1;
    while (path[k] && k < 127) {
        if (path[k] == '/') {
            memcpy(part, path, k); part[k] = 0;
            i = f_find(e, part);
            if (i >= 0 && (e->fflag[i] & F_DIR) && !(e->fflag[i] & F_OPEN)) f_open_dir(e, i);
        }
        ++k;
    }
    e->ffollow = 1;
}

// A file just written: into the tree, in its place among its folder's
// files, if that folder is open and it is not there yet.
static void f_add(struct edit *e, char *path) {
    char dir[128];
    int p, d, i, k;
    if (!e->fpath || path[0] != '/' || f_find(e, path) >= 0) return;
    k = ui_len(path);
    while (k > 0 && path[k - 1] != '/') --k;
    if (k <= 1) { p = 0 - 1; d = 0; }
    else {
        if (k > 127) return;
        memcpy(dir, path, k - 1); dir[k - 1] = 0;
        p = f_find(e, dir);
        if (p < 0 || !(e->fflag[p] & F_OPEN)) return;
        d = e->fdepth[p] + 1;
    }
    i = p + 1;
    while (i < e->fn && e->fdepth[i] >= d) {
        if (e->fdepth[i] == d && !(e->fflag[i] & F_DIR) && f_before(path + k, e->fpath[i] + k)) break;
        ++i;
    }
    f_ins(e, i, path, d, 0);
}

// The pane's widest row, in 6-pixel steps; wider than the pane, it gets a
// bar across, which takes a row.
static int f_maxw(struct edit *e) {
    int i, k, w, mx;
    char *p;
    mx = 0; i = 0;
    while (i < e->fn) {
        p = e->fpath[i];
        k = ui_len(p);
        w = k;
        while (w > 0 && p[w - 1] != '/') --w;
        w = (4 + e->fdepth[i] * 12 + 28) / 6 + (k - w) + 1;
        if (w > mx) mx = w;
        ++i;
    }
    return mx;
}
static int f_hvis() { return (FW - 17) / 6; }
static int f_hbar(struct edit *e) { return f_maxw(e) > f_hvis(); }
static int f_rows(struct edit *e, int ch) { return (ch - FHDR - 2 - (f_hbar(e) ? 16 : 0)) / FROW; }
static void f_clamp(struct edit *e, int rows) {
    int max;
    if (e->ftop > e->fn - rows) e->ftop = e->fn - rows;
    if (e->ftop < 0) e->ftop = 0;
    max = f_maxw(e) - f_hvis();
    if (max < 0) max = 0;
    if (e->fleft > max) e->fleft = max;
    if (e->fleft < 0) e->fleft = 0;
}
// the pane is drawn only where the text keeps some room
static int f_on(struct edit *e, int cw) { return e->fshown && e->fpath && cw >= FW + 200; }

static void f_draw(struct edit *e, int ox, int oy, int ch) {
    int rows, i, y, x, cur, k, hb, vb, dx;
    char *p;
    rows = f_rows(e, ch);
    hb = f_hbar(e);
    vb = e->fn > rows;
    cur = f_find(e, e->path);
    if (e->ffollow && cur >= 0) {
        if (cur < e->ftop) e->ftop = cur;
        if (cur >= e->ftop + rows) e->ftop = cur - rows / 2;
    }
    e->ffollow = 0;
    f_clamp(e, rows);
    dx = e->fleft * 6;
    d_rect(ox, oy, FW, ch, 0xf4f4f4);
    d_rect(ox, oy, FW, FHDR, C_FACE);
    d_bevel(ox, oy, FW, FHDR, B_RAISED);
    d_text(ox + 6, oy + 4, C_DARK, F_BOLD, "Files");
    d_clip(ox, oy + FHDR, FW - (vb ? 17 : 0), ch - FHDR - (hb ? 16 : 0));
    i = e->ftop;
    y = oy + FHDR + 2;
    while (i < e->fn && i < e->ftop + rows) {
        x = ox + 4 + e->fdepth[i] * 12 - dx;
        if (i == cur) d_rect(ox, y, FW, FROW, 0xc8dcff);
        if (e->fflag[i] & F_DIR) {
            if (e->fflag[i] & F_OPEN) { d_line(x, y + 6, x + 4, y + 10, C_DARK); d_line(x + 4, y + 10, x + 8, y + 6, C_DARK); }
            else { d_line(x + 2, y + 4, x + 6, y + 8, C_DARK); d_line(x + 6, y + 8, x + 2, y + 12, C_DARK); }
            d_rect(x + 11, y + 3, 5, 2, 0xc8a000);
            d_rect(x + 11, y + 5, 13, 8, 0xffd84a);
        } else {
            d_rect(x + 13, y + 2, 9, 12, C_WHITE);
            d_recto(x + 13, y + 2, 9, 12, C_SHADOW);
        }
        p = e->fpath[i];
        k = ui_len(p);
        while (k > 0 && p[k - 1] != '/') --k;
        d_text(x + 28, y + 2, C_DARK, F_SANS, p + k);
        y = y + FROW;
        ++i;
    }
    d_clip_end();
    if (vb) ui_scrollbar(ox + FW - 17, oy + FHDR, ch - FHDR - (hb ? 16 : 0), e->fn, rows, e->ftop);
    if (hb) ui_hscrollbar(ox, oy + ch - 16, FW - 17, f_maxw(e), f_hvis(), e->fleft);
    d_rect(ox + FW, oy, 2, ch, C_SHADOW);
}

// a file picked in the pane: into the editor, unless that loses changes
static void f_pick(struct win *w, int i) {
    struct edit *e;
    char p[128];
    e = E(w);
    if (ui_eq(e->fpath[i], e->path)) return;
    if (e->modified) { e_say(e, "No write since last change: :w first, or :e! FILE"); return; }
    ui_cpy(p, e->fpath[i]);
    if (!e_open(w, p, 0)) e_say(e, "Cannot open that file");
    e_retitle(w);
}

static void f_mouse(struct win *w, int ev, int x, int y, int ch) {
    struct edit *e;
    int rows, t, i, hb;
    e = E(w);
    rows = f_rows(e, ch);
    hb = f_hbar(e);
    if (ev == E_WHEEL) { e->ftop = e->ftop + y * 3; f_clamp(e, rows); return; }
    if (ev != E_DOWN) return;
    if (e->fn > rows && x >= FW - 17) {
        t = ui_scroll_click(x, y, FW - 17, FHDR, ch - FHDR - (hb ? 16 : 0), e->fn, rows, e->ftop);
        if (t >= 0) { e->ftop = t; return; }
    }
    if (hb && y >= ch - 16) {
        t = ui_hscroll_click(x, y, 0, ch - 16, FW - 17, f_maxw(e), f_hvis(), e->fleft);
        if (t >= 0) e->fleft = t;
        return;
    }
    if (y < FHDR + 2 || x >= FW) return;
    i = e->ftop + (y - FHDR - 2) / FROW;
    if (i < 0 || i >= e->fn || i >= e->ftop + rows) return;
    if (e->fflag[i] & F_DIR) { if (e->fflag[i] & F_OPEN) f_close_dir(e, i); else f_open_dir(e, i); }
    else f_pick(w, i);
}

// ---- drawing ------------------------------------------------------------------------------

static int e_selected(struct edit *e, int y, int x) {
    int y0, x0, y1, x1;
    if (!e_sel_range(e, &y0, &x0, &y1, &x1)) return 0;
    if (y < y0 || y > y1) return 0;
    if (y == y0 && x < x0) return 0;
    if (y == y1 && x >= x1) return 0;
    return 1;
}

// Rows line Y takes when wrapped at COLS: one at least
static int e_wh(struct edit *e, int y, int cols) {
    int h;
    h = (e->len[y] + cols - 1) / cols;
    return h < 1 ? 1 : h;
}
// the wrapped row of line Y the cursor's column falls in
static int e_wseg(struct edit *e, int y, int x, int cols) {
    int s, h;
    s = x / cols;
    h = e_wh(e, y, cols);
    return s < h ? s : h - 1;
}

enum { BHDR = ECH + 2, BLOG_H = BROWS * ECH + 2, BPANE = BHDR + BLOG_H + 16 };

static void e_draw_text(struct win *w, int ox, int oy, int cw, int ch, int focused) {
    struct edit *e;
    int rows, cols, gw, y, line, x, n, sx, ph, sy, k, i, c, endsel, lang, st, tah, hb, h, seg, a, cseg, r, used, l, by;
    char num[16];
    e = E(w);
    ec = scheme_cols + ide_scheme * SC_N;
    ph = e->bshown ? BPANE : 0;
    sy = oy + ch - ECH - 4;                       // the status line
    tah = ch - ECH - 4 - ph;                      // the text's area, its bars included
    hb = !e->wrap;
    rows = (tah - 6 - (hb ? 16 : 0)) / ECH;
    if (rows < 1) rows = 1;
    gw = e->nu ? 5 * ECW : 0;
    cols = (cw - 8 - gw - 17) / ECW;
    if (cols < 1) cols = 1;
    e->rows = rows; e->cols = cols; e->gw = gw; e->tah = tah; e->ph = ph; e->hb = hb;
    e->maxw = 0;
    i = 0;
    while (i < e->nl) { if (e->len[i] > e->maxw) e->maxw = e->len[i]; ++i; }
    if (e->cy < e->top) e->top = e->cy;
    if (!e->wrap) {
        if (e->cy >= e->top + rows) e->top = e->cy - rows + 1;
        // across, the view follows the cursor only when it has moved, so the
        // bar can be scrolled away from it
        if (e->cx != e->lcx || e->cy != e->lcy) {
            if (e->cx < e->left) e->left = e->cx;
            if (e->cx >= e->left + cols) e->left = e->cx - cols + 1;
        }
        if (e->left > e->maxw + 1 - cols) e->left = e->maxw + 1 - cols;
        if (e->left < 0) e->left = 0;
    } else {
        e->left = 0;
        // down: the rows from the top line to the cursor's must fit
        while (e->top < e->cy) {
            used = 0; l = e->top;
            while (l < e->cy && used <= rows) { used = used + e_wh(e, l, cols); ++l; }
            used = used + e_wseg(e, e->cy, e->cx, cols) + 1;
            if (used <= rows) break;
            ++e->top;
        }
    }
    e->lcx = e->cx; e->lcy = e->cy;

    d_rect(ox, oy, cw, tah, ec[SC_BG]);
    if (gw) d_rect(ox, oy, gw + 2, tah, ec[SC_GUT]);
    line = e->top;
    r = 0;
    y = oy + 3;
    lang = e->nosyn ? L_TEXT : e_lang(e->path);
    st = e_hlstate(e, e->top, lang);
    sx = ox + 4 + gw;
    while (line < e->nl && r < rows) {
        // the colours for the whole line, which also say what the next starts in
        st = e_hl(e, line, st, lang);
        h = e->wrap ? e_wh(e, line, cols) : 1;
        cseg = e->wrap ? e_wseg(e, line, e->cx, cols) : 0;
        seg = 0;
        while (seg < h && r < rows) {
            a = e->wrap ? seg * cols : e->left;   // the first column this row shows
            if (gw && seg == 0) {
                ui_num(num, line + 1);
                k = ui_len(num);
                d_textn(ox + 2 + (4 - k) * ECW, y, ec[SC_GUTFG] == SHADOW ? C_SHADOW : ec[SC_GUTFG], EFONT, F_MONO, ECW, num, k);
            }
            // the selection behind the text
            if (e_sel_on(e)) {
                x = a;
                endsel = e->len[line] > a + cols ? a + cols : e->len[line];
                while (x <= endsel && x < a + cols) {
                    if (e_selected(e, line, x)) d_rect(sx + (x - a) * ECW, y, ECW, ECH, ec[SC_SEL]);
                    ++x;
                }
            }
            n = e->len[line] - a;
            if (n > cols) n = cols;
            x = 0;
            while (x < n) {
                c = e_hcol(a + x);
                k = x + 1;
                while (k < n && e_hcol(a + k) == c) ++k;
                d_textn(sx + x * ECW, y, c, EFONT, F_MONO, ECW, e->ln[line] + a + x, k - x);
                x = k;
            }
            // the cursor: a block in Normal, a bar in Insert
            if (focused && line == e->cy && seg == cseg && e->mode != M_CMD) {
                x = sx + (e->cx - a) * ECW;
                if (e->vim && e->mode != M_INSERT) {
                    d_rect(x, y, ECW, ECH, ec[SC_CUR]);
                    c = e_char(e, line, e->cx);
                    if (c != 10) { num[0] = c; d_textn(x, y, ec[SC_CURFG], EFONT, F_MONO, ECW, num, 1); }
                } else d_rect(x, y, 2, ECH - 1, ec[SC_CUR]);
            }
            y = y + ECH;
            ++r; ++seg;
        }
        ++line;
    }
    while (r < rows) { d_text(sx, y, ec[SC_TILDE], F_MONO, "~"); y = y + ECH; ++r; }
    ui_scrollbar(ox + cw - 17, oy, tah - (hb ? 16 : 0), e->nl, rows, e->top);
    if (hb) {
        ui_hscrollbar(ox, oy + tah - 16, cw - 17, e->maxw + 1, cols, e->left);
        d_rect(ox + cw - 17, oy + tah - 16, 17, 16, C_FACE);
    }

    // the build pane: a header with Cancel while it runs, the log, its bars
    if (e->bshown) {
        y = oy + tah;
        d_rect(ox, y, cw, BHDR, C_FACE);
        d_bevel(ox, y, cw, BHDR, B_RAISED);
        ui_cpy(sb, e->building ? "Build: running" : (e->blogn == 0 ? "Build" : (e->bcancel ? "Build: cancelled" : (e->bstatus == 0 ? "Build: finished" : "Build: failed"))));
        if (e->en) { ui_cat(sb, " - "); ui_num(sb + ui_len(sb), e->en); ui_cat(sb, e->en == 1 ? " error (F8)" : " errors (F8)"); }
        d_text(ox + 6, y + 2, C_DARK, F_SANS, sb);
        if (e->building) ui_button(ox + cw - 72, y + 1, 66, BHDR - 2, "Cancel", 0, 0);
        by = y + BHDR;
        e->bcols = (cw - 8 - 17) / ECW;
        if (e->bcols < 1) e->bcols = 1;
        e->bmaxw = 0;
        i = 0;
        while (i < e->blogn) { k = ui_len(e->blog[i]); if (k > e->bmaxw) e->bmaxw = k; ++i; }
        if (e->bleft > e->bmaxw - e->bcols) e->bleft = e->bmaxw - e->bcols;
        if (e->bleft < 0) e->bleft = 0;
        if (e->btop > e->blogn - BROWS) e->btop = e->blogn - BROWS;
        if (e->btop < 0) e->btop = 0;
        d_rect(ox, by, cw - 17, BLOG_H, 0x101010);
        i = e->btop;
        k = 0;
        while (k < BROWS && i < e->blogn) {
            c = 0xc0c0c0;
            n = 0;
            while (n < e->en) { if (e->elog[n] == i) { c = 0xff8080; n = e->en; } ++n; }
            if (i == e->bsel) { d_rect(ox, by + 1 + k * ECH, cw - 17, ECH, 0x304070); c = C_WHITE; }
            n = ui_len(e->blog[i]) - e->bleft;
            if (n > e->bcols) n = e->bcols;
            if (n > 0) d_textn(ox + 4, by + 1 + k * ECH, c, EFONT, F_MONO, ECW, e->blog[i] + e->bleft, n);
            ++k; ++i;
        }
        ui_scrollbar(ox + cw - 17, by, BLOG_H, e->blogn, BROWS, e->btop);
        ui_hscrollbar(ox, by + BLOG_H, cw - 17, e->bmaxw, e->bcols, e->bleft);
        d_rect(ox + cw - 17, by + BLOG_H, 17, 16, C_FACE);
    }

    // the status line: the mode or the command being typed, then where we are
    d_rect(ox, sy, cw, ECH + 4, C_FACE);
    d_bevel(ox, sy, cw, ECH + 4, B_SUNKEN);
    if (e->mode == M_CMD) {
        sb[0] = e->cmdkind; sb[1] = 0;
        ui_cat(sb, e->cmd);
        d_textn(ox + 4, sy + 2, C_DARK, EFONT, F_MONO, ECW, sb, ui_len(sb));
        if (focused) d_rect(ox + 4 + ui_len(sb) * ECW, sy + 2, 2, ECH - 1, C_DARK);
    } else {
        if (e->msg[0]) ui_cpy(sb, e->msg);
        else if (!e->vim) ui_cpy(sb, "-- EDIT --  (Esc for a command: w, make, e FILE; Ctrl+S saves)");
        else if (e->mode == M_INSERT) ui_cpy(sb, "-- INSERT --");
        else if (e->mode == M_VISUAL) ui_cpy(sb, "-- VISUAL --");
        else if (e->mode == M_VLINE) ui_cpy(sb, "-- VISUAL LINE --");
        else sb[0] = 0;
        d_text(ox + 4, sy + 3, C_DARK, F_SANS, sb);
    }
    ui_num(sb, e->cy + 1);
    ui_cat(sb, ",");
    ui_num(sb + ui_len(sb), e->cx + 1);
    k = ui_len(sb);
    d_textn(ox + cw - 12 - k * ECW, sy + 2, C_DARK, EFONT, F_MONO, ECW, sb, k);
}

// the view moved to T: the cursor comes along, as the redraw keeps it in view
static void e_scrolled(struct edit *e, int t) {
    e->top = t;
    if (e->top > e->nl - e->rows) e->top = e->nl - e->rows;
    if (e->top < 0) e->top = 0;
    if (e->cy < e->top) e->cy = e->top;
    if (e->cy >= e->top + e->rows) e->cy = e->top + e->rows - 1;
    e_clamp(e);
}

// The line and column at (x, y) in the text (as the last draw laid it
// out), clamped to the text
static void e_pos_at(struct edit *e, int x, int y, int *py, int *px) {
    int r, l, h;
    r = (y - 3) / ECH;
    if (y < 3 || r < 0) r = 0;
    if (!e->wrap) {
        l = e->top + r;
        if (l >= e->nl) l = e->nl - 1;
        if (l < 0) l = 0;
        *py = l;
        *px = e->left + (x - 4 - e->gw + ECW / 2) / ECW;
    } else {
        // the row's line, and which of its wrapped rows
        l = e->top;
        while (l < e->nl - 1) { h = e_wh(e, l, e->cols); if (r < h) break; r = r - h; ++l; }
        h = e_wh(e, l, e->cols);
        if (r >= h) r = h - 1;
        *py = l;
        *px = r * e->cols + (x - 4 - e->gw + ECW / 2) / ECW;
    }
    if (x - 4 - e->gw + ECW / 2 < 0) *px = e->wrap ? r * e->cols : e->left;
    if (*px > e->len[*py]) *px = e->len[*py];
    if (*px < 0) *px = 0;
}

// Where things are, in the text's own coordinates (as the last draw left
// them): the text's bars, then the build pane's header, log and bars.
static void e_mouse_text(struct win *w, int ev, int x, int y, int cw, int ch) {
    struct edit *e;
    int row, line, t, by, r, l, h;
    e = E(w);
    by = e->tah + BHDR;                            // the build log's top
    if (ev == E_WHEEL) { e_scrolled(e, e->top + y * 3); return; }
    if (ev == E_MOVE) {
        t = ui_scroll_drag(cw - 17, 0, y);
        if (t >= 0) { e_scrolled(e, t); return; }
        t = ui_scroll_drag(0, e->tah - 16, x);
        if (t >= 0) { e->left = t; return; }
        t = ui_scroll_drag(cw - 17, by, y);
        if (t >= 0) { e->btop = t; return; }
        t = ui_scroll_drag(0, by + BLOG_H, x);
        if (t >= 0) { e->bleft = t; return; }
        // a selection being dragged out: Traditional's, or VIM's Visual
        if (e->drag) {
            e_pos_at(e, x, y, &row, &line);
            if (row == e->cy && line == e->cx) return;
            e->cy = row; e->cx = line; e->want = line;
            if (!e->vim) e->tsel = e->vy != e->cy || e->vx != e->cx;
            else { if (e->mode != M_VISUAL) e->mode = M_VISUAL; e_clamp(e); }
            wk_invalidate(w->id, 0, 0, 0, 0);
        }
        return;
    }
    if (ev != E_DOWN && ev != E_DBL) return;
    if (e->bshown && y >= e->tah && y < e->tah + e->ph) {
        if (ev != E_DOWN && y < by) return;
        if (y < by) {
            if (e->building && ui_hit(x, y, cw - 72, e->tah + 1, 66, BHDR - 2)) e_cancel(w);
            return;
        }
        if (y >= by + BLOG_H) {
            if (ev != E_DOWN) return;
            t = ui_hscroll_click(x, y, 0, by + BLOG_H, cw - 17, e->bmaxw, e->bcols, e->bleft);
            if (t >= 0) e->bleft = t;
            return;
        }
        if (x >= cw - 17) {
            if (ev != E_DOWN) return;
            t = ui_scroll_click(x, y, cw - 17, by, BLOG_H, e->blogn, BROWS, e->btop);
            if (t >= 0) e->btop = t;
            return;
        }
        row = e->btop + (y - by - 1) / ECH;
        if (row < 0 || row >= e->blogn) return;
        e->bsel = row;
        if (ev == E_DBL) {
            t = 0;
            while (t < e->en) { if (e->elog[t] == row) { e_goto_error(w, t); return; } ++t; }
        }
        return;
    }
    if (y >= e->tah) return;
    t = ui_scroll_click(x, y, cw - 17, 0, e->tah - (e->hb ? 16 : 0), e->nl, e->rows, e->top);
    if (t >= 0) { e_scrolled(e, t); return; }
    if (e->hb && y >= e->tah - 16) {
        t = ui_hscroll_click(x, y, 0, e->tah - 16, cw - 17, e->maxw + 1, e->cols, e->left);
        if (t >= 0) e->left = t;
        return;
    }
    if (e->mode == M_VISUAL || e->mode == M_VLINE || e->mode == M_CMD) e->mode = e->vim ? M_NORMAL : M_INSERT;
    e->tlast = 0;
    // Shift+click: Traditional's selection runs from where the cursor was
    if (!e->vim && (wk.mods & M_SHIFT) && ev == E_DOWN) { if (!e->tsel) { e->vy = e->cy; e->vx = e->cx; } }
    else e->tsel = 0;
    e_pos_at(e, x, y, &e->cy, &e->cx);
    if (e->vim) e_clamp(e);
    e->want = e->cx;
    if (!e->vim && (wk.mods & M_SHIFT) && ev == E_DOWN) { e->tsel = e->vy != e->cy || e->vx != e->cx; e->drag = 1; return; }
    // a double click takes a word (Traditional); a press starts a selection a drag makes
    if (!e->vim && ev == E_DBL && e_isword(e_char(e, e->cy, e->cx))) {
        r = e->cx; while (r > 0 && e_isword(e->ln[e->cy][r - 1])) --r;
        l = e->cx; while (l < e->len[e->cy] && e_isword(e->ln[e->cy][l])) ++l;
        e->vy = e->cy; e->vx = r; e->cx = l; e->tsel = l > r;
        return;
    }
    e->vy = e->cy; e->vx = e->cx;
    e->drag = ev == E_DOWN;
}

// The pointer over the window: a text beam over the text, an arrow over
// the pane, the bars and the build log (as the last draw laid them out)
int edit_pointer(struct win *w, int x, int y, int cw, int ch) {
    struct edit *e;
    e = E(w);
    if (f_on(e, cw)) {
        if (x < FW + 2) return PTR_ARROW;
        x = x - FW - 2; cw = cw - FW - 2;
    }
    if (y < e->tah - (e->hb ? 16 : 0) && x < cw - 17) return PTR_TEXT;
    return PTR_ARROW;
}

// The file pane on the left when it is shown, the text beside it
void edit_draw(struct win *w, int ox, int oy, int cw, int ch, int focused) {
    struct edit *e;
    e = E(w);
    if (!f_on(e, cw)) { e_draw_text(w, ox, oy, cw, ch, focused); return; }
    f_draw(e, ox, oy, ch);
    d_clip(ox + FW + 2, oy, cw - FW - 2, ch);
    e_draw_text(w, ox + FW + 2, oy, cw - FW - 2, ch, focused);
    d_clip_end();
}

void edit_mouse(struct win *w, int ev, int x, int y, int cw, int ch) {
    struct edit *e;
    int t;
    e = E(w);
    if (ev == E_UP) { e->drag = 0; return; }
    if (!f_on(e, cw)) { e_mouse_text(w, ev, x, y, cw, ch); return; }
    if (ev == E_MOVE) {
        t = ui_scroll_drag(FW - 17, FHDR, y);
        if (t >= 0) { e->ftop = t; return; }
        t = ui_scroll_drag(0, ch - 16, x);
        if (t >= 0) { e->fleft = t; return; }
    } else if (x < FW + 2) { e->drag = 0; f_mouse(w, ev, x, y, ch); return; }
    e_mouse_text(w, ev, x - FW - 2, y, cw - FW - 2, ch);
}

// ---- Settings: the colour scheme and the input mode ------------------------------------------
//
// One page for every IDE window, a window of its own: each choice is used
// at once, in every IDE window open, and kept in /etc/ide.cfg as
// "scheme N input M" (M 1 for VIM), as Settings keeps the desktop's in
// /etc/desktop.cfg.

static char icfg[64];

static void ide_load() {
    int fd, n, i, v;
    ide_loaded = 1;
    if ((fd = uopen("/etc/ide.cfg", O_RD)) < 0) return;
    n = uread(fd, icfg, 60);
    uclose(fd);
    if (n <= 0) return;
    icfg[n] = 0;
    i = 0;
    while (i < n) {
        if (icfg[i] == 's' && icfg[i + 1] == 'c' && icfg[i + 6] == ' ' && e_digits(icfg + i + 7, &v) > 0 && v < N_SCHEMES) ide_scheme = v;
        if (icfg[i] == 'i' && icfg[i + 1] == 'n' && icfg[i + 5] == ' ' && e_digits(icfg + i + 6, &v) > 0) ide_vim = v != 0;
        ++i;
    }
}

static void ide_save() {
    int fd;
    if ((fd = uopen("/etc/ide.cfg", O_WR + O_CREATE + O_TRUNCATE)) < 0) return;
    ui_cpy(icfg, "scheme ");
    ui_num(icfg + ui_len(icfg), ide_scheme);
    ui_cat(icfg, ide_vim ? " input 1\n" : " input 0\n");
    write(fd, icfg, ui_len(icfg));
    uclose(fd);
}

// a choice made: into every IDE window, and kept
static void ide_set(int scheme, int vim) {
    struct win *w;
    struct edit *e;
    ide_scheme = scheme; ide_vim = vim;
    ide_save();
    w = win_first();
    while (w) {
        if (w->used && w->kind == K_EDIT && (e = E(w)) && e->vim != vim) {
            e->vim = vim; e->tsel = 0; e->op = 0; e->pre = 0; e->count = 0;
            e->mode = vim ? M_NORMAL : M_INSERT;
            e_clamp(e);
            e_say(e, vim ? "VIM mode: Esc for Normal, i to type" : "Traditional: type to edit; Esc for a command");
        }
        w = w->next;
    }
    mark_dirty();
}

// a piece of the preview: n characters of s in colour hc, along from *x
static void iset_piece(int *x, int y, int hc, char *s) {
    d_textn(*x, y, ec[hc], EFONT, F_MONO, ECW, s, ui_len(s));
    *x = *x + ui_len(s) * ECW;
}

static void iset_draw(int w, int cw, int ch, int focused) {
    int i, x, y;
    d_rect(0, 0, cw, ch, C_FACE);
    ui_group(8, 8, cw - 16, 66, "Colour scheme");
    i = 0;
    while (i < N_SCHEMES) { ui_radio(18, 28 + i * 20, scheme_names[i], i == ide_scheme); ++i; }
    ui_group(8, 82, cw - 16, 66, "Input mode");
    ui_radio(18, 102, "Traditional: type at once, as in Text Editor", !ide_vim);
    ui_radio(18, 122, "VIM: Normal, Insert and Visual modes", ide_vim);
    // the preview, in the scheme chosen
    ec = scheme_cols + ide_scheme * SC_N;
    y = 156;
    d_rect(8, y, cw - 16, 3 * ECH + 8, ec[SC_BG]);
    d_bevel(8, y, cw - 16, 3 * ECH + 8, B_SUNKEN);
    x = 14; iset_piece(&x, y + 4, HC_PRE, "#include"); iset_piece(&x, y + 4, HC_TEXT, " "); iset_piece(&x, y + 4, HC_STR, "\"c4ix.h\"");
    x = 14; iset_piece(&x, y + 4 + ECH, HC_TYPE, "int"); iset_piece(&x, y + 4 + ECH, HC_TEXT, " main() { ");
    iset_piece(&x, y + 4 + ECH, HC_KW, "return"); iset_piece(&x, y + 4 + ECH, HC_TEXT, " "); iset_piece(&x, y + 4 + ECH, HC_NUM, "0");
    iset_piece(&x, y + 4 + ECH, HC_TEXT, "; }");
    x = 14; iset_piece(&x, y + 4 + 2 * ECH, HC_COM, "// the IDE's colours");
    ui_button(cw - 88, ch - 34, 76, 24, "OK", 0, 1);
}

static void iset_mouse(int w, int ev, int x, int y, int b) {
    int cw, ch, i;
    if (ev != E_DOWN) return;
    wk_size(w, &cw, &ch);
    i = 0;
    while (i < N_SCHEMES) { if (ui_hit(x, y, 14, 26 + i * 20, cw - 28, 18)) { ide_set(i, ide_vim); return; } ++i; }
    if (ui_hit(x, y, 14, 100, cw - 28, 18)) { ide_set(ide_scheme, 0); return; }
    if (ui_hit(x, y, 14, 120, cw - 28, 18)) { ide_set(ide_scheme, 1); return; }
    if (ui_hit(x, y, cw - 88, ch - 34, 76, 24)) wk_close(w);
}

static void iset_key(int w, int down, int code, int ch, int mods) {
    if (!down) return;
    if (code == KEY_ESC || code == KEY_ENTER) wk_close(w);
    else if (code == KEY_UP && ide_scheme > 0) ide_set(ide_scheme - 1, ide_vim);
    else if (code == KEY_DOWN && ide_scheme < N_SCHEMES - 1) ide_set(ide_scheme + 1, ide_vim);
}

static struct wk_class iset_class = { .kind = K_SETTINGS, .draw = iset_draw, .mouse = iset_mouse, .key = iset_key };

static void ide_settings() {
    int i;
    if ((i = wk_find(&iset_class))) { wk_raise(i); return; }
    iset_class.size = 16;
    wk_open(&iset_class, "IDE Settings", 380, 250, WK_FIXED | WK_CENTER, 0);
}

// ---- the window -----------------------------------------------------------------------------

int edit_open(char *path) {
    int i;
    struct win *w;
    struct edit *e;
    if (!ide_loaded) ide_load();
    i = win_open(K_EDIT, "Untitled - IDE", 760, 500, sizeof(struct edit));
    if (i < 0) return i;
    w = win_get(i);
    e = E(w);
    edit_menus[0].title = "File"; edit_menus[1].title = "Edit"; edit_menus[2].title = "Build";
    edit_menus[3].title = "View"; edit_menus[4].title = "Help";
    edit_file_items[0] = "New"; edit_file_items[1] = "Open..."; edit_file_items[2] = "Save";
    edit_file_items[3] = "Save As..."; edit_file_items[4] = "-"; edit_file_items[5] = "Close"; edit_file_items[6] = 0;
    edit_edit_items[E_CUT] = "Cut   Ctrl+X"; edit_edit_items[E_COPY] = "Copy   Ctrl+C"; edit_edit_items[E_PASTE] = "Paste   Ctrl+V";
    edit_edit_items[3] = "-"; edit_edit_items[E_ALL] = "Select All   Ctrl+A"; edit_edit_items[5] = 0;
    edit_build_items[0] = "Build   F5"; edit_build_items[1] = "Run   F6"; edit_build_items[2] = "Next Error   F8";
    edit_build_items[3] = "Previous Error"; edit_build_items[4] = "Show Output   F4";
    edit_build_items[5] = "Cancel Build"; edit_build_items[6] = 0;
    edit_view_items[0] = "Files   Ctrl+B"; edit_view_items[1] = "Syntax Colouring";
    edit_view_items[2] = "Word Wrap"; edit_view_items[3] = "-"; edit_view_items[4] = "Settings..."; edit_view_items[5] = 0;
    edit_help_items[0] = "Keys"; edit_help_items[1] = 0;
    edit_menus[0].items = edit_file_items; edit_menus[1].items = edit_edit_items; edit_menus[2].items = edit_build_items;
    edit_menus[3].items = edit_view_items; edit_menus[4].items = edit_help_items;
    wk_menu(i, edit_menus, 5);
    e->utext = (char **)ualloc(UNDO_MAX * sizeof(int)); e->ulen = (int *)ualloc(UNDO_MAX * sizeof(int));
    e->ucy = (int *)ualloc(UNDO_MAX * sizeof(int)); e->ucx = (int *)ualloc(UNDO_MAX * sizeof(int));
    e->rtext = (char **)ualloc(UNDO_MAX * sizeof(int)); e->rlen = (int *)ualloc(UNDO_MAX * sizeof(int));
    e->rcy = (int *)ualloc(UNDO_MAX * sizeof(int)); e->rcx = (int *)ualloc(UNDO_MAX * sizeof(int));
    e->rec = (int *)ualloc(REC_MAX * 3 * sizeof(int));
    e->dot = (int *)ualloc(REC_MAX * 3 * sizeof(int));
    e->blog = (char **)ualloc(BLOG_MAX * sizeof(int));
    e->epath = (char **)ualloc(ERR_MAX * sizeof(int));
    e->eline = (int *)ualloc(ERR_MAX * sizeof(int));
    e->elog = (int *)ualloc(ERR_MAX * sizeof(int));
    e->vim = ide_vim; e->nu = 1; e->mode = ide_vim ? M_NORMAL : M_INSERT; e->ecur = 0 - 1; e->bsel = 0 - 1;
    e->wrap = 1;
    e->hl_line = 0 - 1;
    e->lcx = 0 - 1; e->lcy = 0 - 1;
    e_insline(e, 0, "", 0);
    // the file pane: the top of the disk, every folder closed
    e->fshown = 1;
    f_reserve(e, 64);
    f_open_dir(e, 0 - 1);
    if (path && path[0]) {
        if (!e_open(w, path, 0)) { ui_ncpy(e->path, path, 128); e_say(e, "New file"); }
    }
    e->modified = 0;
    if (!e->msg[0] && e->vim) e_say(e, "VIM keys: i to type, Esc, :w to save, :make (F5) to build, :help is the Help menu");
    if (!e->msg[0]) e_say(e, "Type to edit; Esc for a command (w, make, e FILE); F5 builds; View > Settings for colours and VIM");
    e_retitle(w);
    return i;
}

// Open and Save As: the kit's file box, starting at the file's own folder
static void e_filebox(struct win *w, int save) {
    struct edit *e;
    e = E(w);
    ui_ncpy(e->ask, e->path[0] ? e->path : (save ? "/tmp/untitled.c" : "/usr"), WK_PATH);
    if (!e->path[0] && !save) ui_cat(e->ask, "/");
    wk_filebox(w->id, save ? A_SAVEAS : A_OPEN, save, ide_filter, e->ask, WK_PATH);
}

void edit_command(struct win *w, int menu, int item) {
    struct edit *e;
    e = E(w);
    if (menu == 0) {
        if (item == 0) {
            if (e->modified) { msgbox(w, "IDE", "Save the changes first (Ctrl+S, or :w), or :q! to drop them.", 0, A_NONE); return; }
            e_settext(e, "", 0); e->path[0] = 0; e->modified = 0; e->cy = 0; e->cx = 0; e->tsel = 0; e_retitle(w);
        }
        else if (item == 1) e_filebox(w, 0);
        else if (item == 2) {
            if (!e->path[0]) e_filebox(w, 1);
            else { e_save(e, e->path); e_retitle(w); }
        }
        else if (item == 3) e_filebox(w, 1);
        else if (item == 5) win_close(win_index(w));
        return;
    }
    if (menu == 1) { e_edit(w, item); return; }
    if (menu == 2) {
        if (item == 0) e_make(w);
        else if (item == 1) e_ex(w, "run");
        else if (item == 2) e_goto_error(w, e->ecur + 1);
        else if (item == 3) e_goto_error(w, e->ecur - 1);
        else if (item == 4) e->bshown = !e->bshown;
        else if (item == 5) e_cancel(w);
        return;
    }
    if (menu == 3) {
        if (item == 0) e->fshown = !e->fshown;
        else if (item == 1) e->nosyn = !e->nosyn;
        else if (item == 2) { e->wrap = !e->wrap; e->left = 0; }
        else if (item == 4) ide_settings();
        return;
    }
    if (e->vim) msgbox(w, "IDE keys",
           "Esc for Normal mode, i to insert. hjkl or arrows move; w b e words; dd yy p; u undo; . repeat. :w save, :make or F5 build, F8 next error, F6 run, Ctrl+B the file pane, :syntax off, :set novim for a plain editor; View > Settings for Traditional typing.",
           0, A_NONE);
    else msgbox(w, "IDE keys",
           "Type to edit; Shift with the arrows selects; Ctrl+X, C, V cut, copy and paste; Ctrl+A all; Ctrl+Z undo, Ctrl+Y redo; Ctrl+S saves. Esc for a command: w, make, run, e FILE, set nowrap. F5 builds, F6 runs, F8 the next error, Ctrl+B the file pane; View > Settings for VIM mode and colours.",
           0, A_NONE);
}

void edit_answer(struct win *w, int action, int yes) {
    struct edit *e;
    e = E(w);
    if (!yes) return;
    if (action == A_OPEN) {
        if (e->modified) { e_say(e, "No write since last change: save first (Ctrl+S, or :w)"); return; }
        if (!e_open(w, e->ask, 0)) { ui_ncpy(e->path, e->ask, 128); e_settext(e, "", 0); e->modified = 0; e_say(e, "New file"); }
    } else if (action == A_SAVEAS) e_save(e, e->ask);
    else if (action == A_BINARY && !e_load(e, e->pend)) e_say(e, "Cannot open that file");
    e->tsel = 0;
    e_retitle(w);
}

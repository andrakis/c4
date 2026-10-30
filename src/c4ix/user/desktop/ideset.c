// ideset.c -- IDE Settings: the colour scheme, the input mode and the
// font. docs/c4ix-desktop.md (parts fifteen and twenty).
//
// One page for every IDE window, a window of its own: each choice is used
// at once, in every IDE window open, and kept in /etc/ide.cfg as
// "scheme N input M font F size S" (M 1 for VIM; F and S the kit's
// fixed-width faces and sizes), as Settings keeps the desktop's in
// /etc/desktop.cfg. It left edit.c, the biggest of the desktop's sources,
// to keep that within what c4sc can compile inside C4IX (as hl.c did).

#include "desktop.h"

// The text grid: a cell's width and height, the font's size and face.
// The font sets them (the kit's grids, wk_grid), for every IDE window
// and its Code tab (edit.c, explore.c)
int ide_cw = 7;
int ide_ch = 15;
int ide_fs = 12;
int ide_face = F_MONO;
#define ECW ide_cw
#define ECH ide_ch
#define EFONT ide_fs
#define EFACE ide_face

// The colour schemes: the text's colours by HC_*, then the rest. A gutter
// number colour of SHADOW is the desktop scheme's shadow.
enum { N_SCHEMES = 2 };
static char *scheme_names[N_SCHEMES] = { "Blue (after VIM's blue)", "Classic (black on white)" };
static int scheme_cols[N_SCHEMES * SC_N] = {
    // Blue: VIM's `blue`, gold on dark blue
    0xffd700, 0xffffff, 0x87d7ff, 0x87ffaf, 0xd7d7af, 0xff87af, 0x87ffff,
    0x000087, 0x00005f, 0xffff87, 0x5f87d7, 0x5f87ff, 0xffd700, 0x000087,
    // Classic: the IDE's first colours
    0x000000, 0x0000b0, 0x007878, 0xa01818, 0x2a8a2a, 0x905000, 0x8020a0,
    0xffffff, 0xe8e8e8, SHADOW, 0xa8c8ff, 0x6060c0, 0x000000, 0xffffff };
// Settings: the scheme, the input mode (VIM's or not) and the font's
// face and size, for every IDE window, kept in /etc/ide.cfg
static int ide_scheme, ide_vim, ide_loaded, ide_mono;
static int ide_size = 1;
static int *ec;                       // the scheme the preview is drawn in

static char icfg[64];

// the number at S, into *V: how many digits it had
static int digits(char *s, int *v) {
    int k;
    k = 0; *v = 0;
    while (s[k] >= '0' && s[k] <= '9') { *v = *v * 10 + (s[k] - '0'); ++k; }
    return k;
}

// the grid the font makes
static void ide_grid() {
    ide_face = wk_mono_font(ide_mono);
    wk_grid(ide_size, &ide_fs, &ide_cw, &ide_ch);
}

static void ide_load() {
    int fd, n, i, v;
    ide_loaded = 1;
    n = 0;
    if ((fd = uopen("/etc/ide.cfg", O_RD)) >= 0) { n = uread(fd, icfg, 60); uclose(fd); }
    if (n < 0) n = 0;
    icfg[n] = 0;
    i = 0;
    while (i < n) {
        if (icfg[i] == 's' && icfg[i + 1] == 'c' && icfg[i + 6] == ' ' && digits(icfg + i + 7, &v) > 0 && v < N_SCHEMES) ide_scheme = v;
        if (icfg[i] == 'i' && icfg[i + 1] == 'n' && icfg[i + 5] == ' ' && digits(icfg + i + 6, &v) > 0) ide_vim = v != 0;
        if (icfg[i] == 'f' && icfg[i + 1] == 'o' && icfg[i + 4] == ' ' && digits(icfg + i + 5, &v) > 0 && v < WK_FACES) ide_mono = v;
        if (icfg[i] == 's' && icfg[i + 1] == 'i' && icfg[i + 4] == ' ' && digits(icfg + i + 5, &v) > 0 && v < WK_SIZES) ide_size = v;
        ++i;
    }
    ide_grid();
}

static void ide_save() {
    int fd;
    if ((fd = uopen("/etc/ide.cfg", O_WR + O_CREATE + O_TRUNCATE)) < 0) return;
    ui_cpy(icfg, "scheme ");
    ui_num(icfg + ui_len(icfg), ide_scheme);
    ui_cat(icfg, ide_vim ? " input 1 font " : " input 0 font ");
    ui_num(icfg + ui_len(icfg), ide_mono);
    ui_cat(icfg, " size ");
    ui_num(icfg + ui_len(icfg), ide_size);
    ui_cat(icfg, "\n");
    write(fd, icfg, ui_len(icfg));
    uclose(fd);
}

// what edit.c asks: the scheme's colours, and VIM mode or not, read from
// /etc/ide.cfg the first time
int *ide_colours() {
    if (!ide_loaded) ide_load();
    return scheme_cols + ide_scheme * SC_N;
}
int ide_vim_on() {
    if (!ide_loaded) ide_load();
    return ide_vim;
}

// a choice made: into every IDE window, and kept
static void ide_set(int scheme, int vim) {
    ide_scheme = scheme; ide_vim = vim;
    ide_save();
    edit_vim_all(vim);
    mark_dirty();
}

// the font chosen: every IDE window, and its Code tab, laid out again in it
static void ide_set_font(int face, int size) {
    ide_mono = face; ide_size = size;
    ide_grid();
    ide_save();
    mark_dirty();
}

// a piece of the preview: n characters of s in colour hc, along from *x
static void iset_piece(int *x, int y, int hc, char *s) {
    d_textn(*x, y, ec[hc], EFONT, EFACE, ECW, s, ui_len(s));
    *x = *x + ui_len(s) * ECW;
}

// the font group's columns: a face, or a size, in each
static int iset_col[3] = { 18, 112, 222 };

static void iset_draw(int w, int cw, int ch, int focused) {
    int i, x, y;
    d_rect(0, 0, cw, ch, C_FACE);
    ui_group(8, 8, cw - 16, 66, "Colour scheme");
    i = 0;
    while (i < N_SCHEMES) { ui_radio(18, 28 + i * 20, scheme_names[i], i == ide_scheme); ++i; }
    ui_group(8, 82, cw - 16, 66, "Input mode");
    ui_radio(18, 102, "Traditional: type at once, as in Text Editor", !ide_vim);
    ui_radio(18, 122, "VIM: Normal, Insert and Visual modes", ide_vim);
    ui_group(8, 156, cw - 16, 66, "Font");
    i = 0;
    while (i < WK_FACES) { ui_radio(iset_col[i], 174, wk_mono_name(i), i == ide_mono); ++i; }
    i = 0;
    while (i < WK_SIZES) { ui_radio(iset_col[i], 196, wk_size_name(i), i == ide_size); ++i; }
    // the preview, in the scheme and the font chosen
    ec = scheme_cols + ide_scheme * SC_N;
    y = 230;
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
    i = 0;
    while (i < 3) {
        if (i < WK_FACES && ui_hit(x, y, iset_col[i] - 4, 172, 100, 18)) { ide_set_font(i, ide_size); return; }
        if (i < WK_SIZES && ui_hit(x, y, iset_col[i] - 4, 194, 100, 18)) { ide_set_font(ide_mono, i); return; }
        ++i;
    }
    if (ui_hit(x, y, cw - 88, ch - 34, 76, 24)) wk_close(w);
}

static void iset_key(int w, int down, int code, int ch, int mods) {
    if (!down) return;
    if (code == KEY_ESC || code == KEY_ENTER) wk_close(w);
    else if (code == KEY_UP && ide_scheme > 0) ide_set(ide_scheme - 1, ide_vim);
    else if (code == KEY_DOWN && ide_scheme < N_SCHEMES - 1) ide_set(ide_scheme + 1, ide_vim);
}

static struct wk_class iset_class = { .kind = K_SETTINGS, .draw = iset_draw, .mouse = iset_mouse, .key = iset_key };

void ide_settings() {
    int i;
    if ((i = wk_find(&iset_class))) { wk_raise(i); return; }
    iset_class.size = 16;
    wk_open(&iset_class, "IDE Settings", 380, 370, WK_FIXED | WK_CENTER, 0);
}

// c4tui -- a character-cell UI library for C4DOS.
//
// Include it as a SOURCE FILE, the way include/c4dos.h is used:
//
//     ./c4cc -o app.c4r src/c4tui/c4tui.c app.c
//     ./c4sp src/c4sp/lisp/c4lc.lisp -O -conforming app-with-include.c app.c4r
//
// Dialect: strict c4 -- no structs, no unsigned, declarations at the top
// of a function -- so it builds with c4cc as well as with c4lc/c4fc.
// The one thing it does want is -conforming, because it writes \033.
//
// WHAT IT IS. A screen of cells, each a character and an attribute, and
// a flush that emits only what CHANGED since the last one. Everything
// else -- boxes, menus, dialogs, fields -- writes into that grid and is
// therefore free until the flush.
//
// WHY DAMAGE TRACKING IS THE WHOLE DESIGN. raycast repaints all 2000
// cells every frame because every cell really does change. An editor is
// the opposite: a keystroke changes one line, often one cell. Emitting
// the whole screen would cost 2000 characters through a formatter that
// runs 15-20 instructions per character; emitting the damage costs
// tens. On c4bb that is the difference between an editor that feels
// like an editor and one that does not.
//
// The second decision is the same one raycast made: ONE write per
// frame. plat_emit is PUTS on c4m/c4bb, where a whole frame is one
// microcoded instruction, and printf on stock c4, where it is not.
//
// THE CURSOR IS DRAWN, NOT PLACED. The hardware cursor is hidden and
// the caller marks a cell instead (tui_cursor). Two reasons: PUTS
// appends a newline that would move a placed cursor off the cell it was
// just put on, and a drawn block cursor is what a DOS editor looks like
// anyway. tui_hwcursor(1) turns the real one back on if you want it.

// ---- platform ------------------------------------------------------
// puts exists on c4m, c4mp and c4bb; plain c4 has only the nine
// builtins. C4DOS runs on all of them, so the split is a build flag --
// the same one raycast uses.
#ifdef TUI_PLAIN_C4
#define tui_emit(b)  printf("%s\n", (b))
#else
int tui_emit (char *b) { puts(b); return 0; }
#endif

// ---- geometry ------------------------------------------------------
// There is no ioctl and no TIOCGWINSZ anywhere in this family, so the
// size is declared rather than discovered. 80x25 is what C4DOS assumes
// and what c4bb's terminal is.
int tui_w;
int tui_h;

int  tui_ch;      // cells: character
int  tui_at;      // cells: attribute
int  tui_pch;     // what the terminal is currently showing
int  tui_pat;
int  tui_buf;     // the assembled byte stream
int  tui_bufmax;

int tui_curx, tui_cury, tui_curon;   // the DRAWN cursor
int tui_hwcur;                       // is the real cursor visible

// ---- attributes ----------------------------------------------------
// One byte: foreground in the low nibble, background in the high one,
// which is the layout every DOS program used and the reason a colour
// pair fits in a cell beside its character.
enum {
  TUI_BLACK = 0, TUI_BLUE, TUI_GREEN, TUI_CYAN,
  TUI_RED, TUI_MAGENTA, TUI_BROWN, TUI_LGREY,
  TUI_DGREY, TUI_LBLUE, TUI_LGREEN, TUI_LCYAN,
  TUI_LRED, TUI_LMAGENTA, TUI_YELLOW, TUI_WHITE
};

int tui_attr (int fg, int bg) { return (fg & 15) + ((bg & 15) * 16); }

// ANSI orders its colours differently from the DOS palette above:
// DOS is black,blue,green,cyan,red,magenta,brown,grey and ANSI is
// black,red,green,yellow,blue,magenta,cyan,white. This is that
// permutation and nothing more.
int tui_ansi;

// ---- little helpers ------------------------------------------------
// c4 has no string library at all, so these are the ones this file
// needs and no more.
int tui_strlen (char *s) { int n; n = 0; while (*s) { ++n; ++s; } return n; }

char *tui_puts_buf (char *p, char *s) { while (*s) { *p = *s; ++p; ++s; } return p; }

char *tui_putn_buf (char *p, int n) {
  char t[12]; int i;
  if (n == 0) { *p = '0'; return p + 1; }
  i = 0;
  while (n > 0) { t[i] = '0' + n % 10; n = n / 10; ++i; }
  while (i > 0) { --i; *p = t[i]; ++p; }
  return p;
}

// ---- the grid ------------------------------------------------------

int tui_cell (int x, int y) { return y * tui_w + x; }

void tui_put (int x, int y, int c, int a) {
  int i;
  if (x < 0 || y < 0 || x >= tui_w || y >= tui_h) return;
  i = y * tui_w + x;
  *(char *)(tui_ch + i) = c;
  *(char *)(tui_at + i) = a;
}

void tui_fill (int x, int y, int w, int h, int c, int a) {
  int r, col;
  r = 0;
  while (r < h) { col = 0; while (col < w) { tui_put(x + col, y + r, c, a); ++col; } ++r; }
}

void tui_cls (int a) { tui_fill(0, 0, tui_w, tui_h, ' ', a); }

// Text, clipped to the screen. Returns the column just past the end,
// so a caller can chain runs of different attributes.
int tui_text (int x, int y, char *s, int a) {
  while (*s) { tui_put(x, y, *s, a); ++x; ++s; }
  return x;
}

// The same, padded or truncated to exactly w columns -- which is what
// every field, menu item and status line actually wants.
int tui_textf (int x, int y, char *s, int a, int w) {
  int i;
  i = 0;
  while (i < w) {
    if (*s) { tui_put(x + i, y, *s, a); ++s; }
    else      tui_put(x + i, y, ' ', a);
    ++i;
  }
  return x + w;
}

void tui_num (int x, int y, int n, int a) {
  char t[12]; char *e;
  e = tui_putn_buf(t, n); *e = 0;
  tui_text(x, y, t, a);
}

void tui_cursor (int x, int y) { tui_curx = x; tui_cury = y; tui_curon = 1; }
void tui_nocursor ()           { tui_curon = 0; }

// ---- boxes ---------------------------------------------------------
// ASCII by default. c4bb's web terminal stores one byte per cell, so a
// UTF-8 line-drawing character would arrive as three cells of noise;
// tui_boxstyle(1) switches to them anyway for a native run in a UTF-8
// terminal, where they look like the real thing.
void tui_box (int x, int y, int w, int h, int a) {
  int i;
  if (w < 2 || h < 2) return;
  tui_put(x, y, '+', a);  tui_put(x + w - 1, y, '+', a);
  tui_put(x, y + h - 1, '+', a);  tui_put(x + w - 1, y + h - 1, '+', a);
  i = 1;
  while (i < w - 1) { tui_put(x + i, y, '-', a); tui_put(x + i, y + h - 1, '-', a); ++i; }
  i = 1;
  while (i < h - 1) { tui_put(x, y + i, '|', a); tui_put(x + w - 1, y + i, '|', a); ++i; }
}

// A framed, cleared window with a centred title -- the shape every
// dialog in this library and every window in an editor starts from.
void tui_window (int x, int y, int w, int h, char *title, int a) {
  int n;
  tui_fill(x, y, w, h, ' ', a);
  tui_box(x, y, w, h, a);
  if (title) {
    n = tui_strlen(title);
    if (n > w - 4) n = w - 4;
    if (n > 0) {
      tui_put(x + (w - n) / 2 - 1, y, ' ', a);
      tui_text(x + (w - n) / 2, y, title, a);
      tui_put(x + (w - n) / 2 + n, y, ' ', a);
    }
  }
}

// A one-cell shadow, drawn by DARKENING what is already there rather
// than by painting over it -- so it works on any background.
void tui_shadow (int x, int y, int w, int h) {
  int i, j;
  i = 0;
  while (i < w) {
    j = y + h;
    if (j < tui_h && x + i + 1 < tui_w)
      *(char *)(tui_at + tui_cell(x + i + 1, j)) = tui_attr(TUI_DGREY, TUI_BLACK);
    ++i;
  }
  j = 0;
  while (j < h) {
    if (x + w < tui_w && y + j + 1 < tui_h)
      *(char *)(tui_at + tui_cell(x + w, y + j + 1)) = tui_attr(TUI_DGREY, TUI_BLACK);
    ++j;
  }
}

// Save and restore a rectangle, so a popup can put back what it covered
// without the caller having to redraw the world. Returns a malloc'd
// block the caller passes back to tui_restore.
int tui_save (int x, int y, int w, int h) {
  int *s; int r, c, i;
  if (x < 0) x = 0; if (y < 0) y = 0;
  if (x + w > tui_w) w = tui_w - x;
  if (y + h > tui_h) h = tui_h - y;
  if (w <= 0 || h <= 0) return 0;
  s = malloc((w * h * 2 + 4) * sizeof(int));
  if (!s) return 0;
  s[0] = x; s[1] = y; s[2] = w; s[3] = h;
  i = 4; r = 0;
  while (r < h) {
    c = 0;
    while (c < w) {
      s[i] = *(char *)(tui_ch + tui_cell(x + c, y + r));
      s[i + 1] = *(char *)(tui_at + tui_cell(x + c, y + r));
      i = i + 2; ++c;
    }
    ++r;
  }
  return (int)s;
}

void tui_restore (int blk) {
  int *s; int r, c, i, x, y, w, h;
  if (!blk) return;
  s = (int *)blk;
  x = s[0]; y = s[1]; w = s[2]; h = s[3];
  i = 4; r = 0;
  while (r < h) {
    c = 0;
    while (c < w) { tui_put(x + c, y + r, s[i], s[i + 1]); i = i + 2; ++c; }
    ++r;
  }
  free((char *)s);
}

// ---- flushing ------------------------------------------------------
// The only place that talks to the terminal, and the reason everything
// above is free: it walks the grid against what the terminal is already
// showing and emits the RUNS that differ, positioning the cursor once
// per run. A frame that changed one line costs one escape and that
// line; a frame that changed nothing costs nothing at all.

char *tui_sgr (char *p, int a) {
  int fg, bg;
  fg = a & 15; bg = (a / 16) & 15;
  p = tui_puts_buf(p, "\033[0");
  if (fg > 7) p = tui_puts_buf(p, ";1");
  p = tui_puts_buf(p, ";");
  p = tui_putn_buf(p, 30 + *(char *)(tui_ansi + (fg & 7)));
  p = tui_puts_buf(p, ";");
  p = tui_putn_buf(p, 40 + *(char *)(tui_ansi + (bg & 7)));
  p = tui_puts_buf(p, "m");
  return p;
}

char *tui_goto (char *p, int x, int y) {
  p = tui_puts_buf(p, "\033[");
  p = tui_putn_buf(p, y + 1);
  p = tui_puts_buf(p, ";");
  p = tui_putn_buf(p, x + 1);
  p = tui_puts_buf(p, "H");
  return p;
}

void tui_flush () {
  char *p; int y, x, i, a, cura, run, sx, cc, ca, ci;
  p = (char *)tui_buf;
  cura = 0 - 1;                 // impossible: forces an SGR on the first run
  ci = 0 - 1;
  if (tui_curon) ci = tui_cell(tui_curx, tui_cury);

  y = 0;
  while (y < tui_h) {
    x = 0;
    while (x < tui_w) {
      i = y * tui_w + x;
      cc = *(char *)(tui_ch + i);
      ca = *(char *)(tui_at + i);
      // The drawn cursor is a reversed cell. It is applied HERE rather
      // than in the grid so that moving it damages only the two cells
      // involved, and so the caller never has to undo it.
      if (i == ci) ca = tui_attr((ca / 16) & 15, ca & 15);
      if (cc == *(char *)(tui_pch + i) && ca == *(char *)(tui_pat + i)) { ++x; continue; }

      // A run: from here to the last cell on this row that differs.
      sx = x; run = 0;
      while (x < tui_w) {
        i = y * tui_w + x;
        cc = *(char *)(tui_ch + i);
        ca = *(char *)(tui_at + i);
        if (i == ci) ca = tui_attr((ca / 16) & 15, ca & 15);
        if (cc == *(char *)(tui_pch + i) && ca == *(char *)(tui_pat + i)) break;
        ++run; ++x;
      }
      p = tui_goto(p, sx, y);
      i = y * tui_w + sx;
      while (run > 0) {
        cc = *(char *)(tui_ch + i);
        ca = *(char *)(tui_at + i);
        if (i == ci) ca = tui_attr((ca / 16) & 15, ca & 15);
        if (ca != cura) { p = tui_sgr(p, ca); cura = ca; }
        *p = cc; ++p;
        *(char *)(tui_pch + i) = *(char *)(tui_ch + i);
        *(char *)(tui_pat + i) = ca;
        ++i; --run;
      }
    }
    ++y;
  }
  if (p == (char *)tui_buf) return;          // nothing changed: say nothing
  p = tui_puts_buf(p, "\033[0m");
  if (tui_hwcur) p = tui_goto(p, tui_curx, tui_cury);
  *p = 0;
  tui_emit((char *)tui_buf);
}

// Forget what the terminal is showing, so the next flush repaints
// everything. For Ctrl-L, and for coming back from a program that wrote
// to the screen behind our back.
void tui_damage () { memset((char *)tui_pch, 0, tui_w * tui_h);
                     memset((char *)tui_pat, 255, tui_w * tui_h); }

// ---- keyboard ------------------------------------------------------
// Never read fd 0: a blocking read there stops the whole VM. The rule
// in this family is a second descriptor opened non-blocking, and
// /dev/tty rather than /dev/stdin because only the former is raw --
// c4bb's device model draws exactly that distinction.

enum { TUI_O_NONBLOCK = 2048 };   // 0x800; no header in this family has it

int tui_fd;
int tui_rb;         // ring of raw bytes
int tui_rh, tui_rt;
enum { TUI_RING = 256 };

// Key codes. Printable keys are themselves; control keys are 1..31 as
// they arrive; everything else is above 255 so a caller can switch on
// the lot without ambiguity.
enum {
  TUI_NONE = 4095,          // not -1: c4cc's enum parser takes literals only
  TUI_UP = 256, TUI_DOWN, TUI_LEFT, TUI_RIGHT,
  TUI_HOME, TUI_END, TUI_PGUP, TUI_PGDN, TUI_INS, TUI_DEL,
  TUI_F1, TUI_F2, TUI_F3, TUI_F4, TUI_F5, TUI_F6,
  TUI_F7, TUI_F8, TUI_F9, TUI_F10, TUI_F11, TUI_F12,
  TUI_ALT = 512      // TUI_ALT + 'f' is Alt-F, which is how menus open
};
enum { TUI_ESC = 27, TUI_ENTER = 13, TUI_TAB = 9, TUI_BS = 127 };

void tui_kbd_open () {
  tui_fd = open("/dev/tty", TUI_O_NONBLOCK);
  if (tui_fd < 0) tui_fd = open("/dev/stdin", TUI_O_NONBLOCK);
}

// Pull whatever is waiting into the ring. Returns how many bytes.
int tui_poll () {
  char b[64]; int n, i;
  if (tui_fd < 0) return 0;
  n = read(tui_fd, b, 64);
  if (n <= 0) return 0;
  i = 0;
  while (i < n) {
    *(char *)(tui_rb + (tui_rt % TUI_RING)) = b[i];
    ++tui_rt; ++i;
  }
  return n;
}

int tui_ravail () { return tui_rt - tui_rh; }

int tui_rnext () {
  int c;
  if (tui_rh >= tui_rt) return 0 - 1;
  c = *(char *)(tui_rb + (tui_rh % TUI_RING)) & 255;
  ++tui_rh;
  return c;
}

// Wait for at least n bytes to be available, but not forever: an ESC
// that is really the Esc KEY has nothing after it, and the only way to
// tell it from the start of a sequence is to give up. TUI_ESCWAIT polls
// are enough on every host here and cost nothing when a sequence is
// genuinely on its way.
enum { TUI_ESCWAIT = 200 };

int tui_wait (int n) {
  int spins;
  spins = 0;
  while (tui_ravail() < n) {
    if (tui_poll() == 0) { ++spins; if (spins > TUI_ESCWAIT) return 0; }
  }
  return 1;
}

// ESC [ ... final, or ESC O final. Returns a key code, or ESC itself
// when nothing follows.
int tui_escape () {
  int c, n1, n2, got;
  if (!tui_wait(1)) return TUI_ESC;
  c = tui_rnext();
  if (c == 'O') {                       // xterm's application-mode F1-F4
    if (!tui_wait(1)) return TUI_ESC;
    c = tui_rnext();
    if (c == 'P') return TUI_F1;  if (c == 'Q') return TUI_F2;
    if (c == 'R') return TUI_F3;  if (c == 'S') return TUI_F4;
    return TUI_ESC;
  }
  if (c != '[') {
    // ESC followed by an ordinary key is Alt-that-key, which is how a
    // DOS menu bar is opened and why this library bothers.
    if (c >= 'A' && c <= 'Z') c = c + 32;
    return TUI_ALT + c;
  }
  n1 = 0; n2 = 0; got = 0;
  while (1) {
    if (!tui_wait(1)) return TUI_ESC;
    c = tui_rnext();
    if (c >= '0' && c <= '9') { n1 = n1 * 10 + c - '0'; got = 1; continue; }
    if (c == ';') { n2 = n1; n1 = 0; continue; }
    break;
  }
  if (c == 'A') return TUI_UP;     if (c == 'B') return TUI_DOWN;
  if (c == 'C') return TUI_RIGHT;  if (c == 'D') return TUI_LEFT;
  if (c == 'H') return TUI_HOME;   if (c == 'F') return TUI_END;
  if (c == '~') {
    if (!got) return TUI_ESC;
    if (n1 == 1) return TUI_HOME;  if (n1 == 2) return TUI_INS;
    if (n1 == 3) return TUI_DEL;   if (n1 == 4) return TUI_END;
    if (n1 == 5) return TUI_PGUP;  if (n1 == 6) return TUI_PGDN;
    if (n1 == 11) return TUI_F1;   if (n1 == 12) return TUI_F2;
    if (n1 == 13) return TUI_F3;   if (n1 == 14) return TUI_F4;
    if (n1 == 15) return TUI_F5;   if (n1 == 17) return TUI_F6;
    if (n1 == 18) return TUI_F7;   if (n1 == 19) return TUI_F8;
    if (n1 == 20) return TUI_F9;   if (n1 == 21) return TUI_F10;
    if (n1 == 23) return TUI_F11;  if (n1 == 24) return TUI_F12;
  }
  return TUI_ESC;
}

// One key, or TUI_NONE if none is waiting. Never blocks.
int tui_key_nb () {
  int c;
  tui_poll();
  if (tui_ravail() == 0) return TUI_NONE;
  c = tui_rnext();
  if (c == 27) return tui_escape();
  return c;
}

// One key, waiting for it. This is the one an editor's main loop uses.
int tui_key () {
  int c;
  while (1) { c = tui_key_nb(); if (c != TUI_NONE) return c; }
}

// ---- start and stop ------------------------------------------------

int tui_pal;      // the DOS-to-ANSI colour permutation, as bytes

void tui_init (int w, int h) {
  int n;
  tui_w = w; tui_h = h;
  n = w * h;
  tui_ch = (int)malloc(n);   tui_at = (int)malloc(n);
  tui_pch = (int)malloc(n);  tui_pat = (int)malloc(n);
  // Worst case is every cell changed and every cell a different colour:
  // an SGR run is under 16 bytes and a position under 10.
  tui_bufmax = n * 20 + 256;
  tui_buf = (int)malloc(tui_bufmax);
  tui_rb  = (int)malloc(TUI_RING);
  tui_pal = (int)malloc(8);
  // DOS black,blue,green,cyan,red,magenta,brown,grey
  //  -> ANSI  0,   4,    2,    6,   1,      5,      3,    7
  *(char *)(tui_pal + 0) = 0; *(char *)(tui_pal + 1) = 4;
  *(char *)(tui_pal + 2) = 2; *(char *)(tui_pal + 3) = 6;
  *(char *)(tui_pal + 4) = 1; *(char *)(tui_pal + 5) = 5;
  *(char *)(tui_pal + 6) = 3; *(char *)(tui_pal + 7) = 7;
  tui_ansi = tui_pal;
  tui_rh = 0; tui_rt = 0;
  tui_curx = 0; tui_cury = 0; tui_curon = 0; tui_hwcur = 0;
  tui_kbd_open();
  tui_cls(tui_attr(TUI_LGREY, TUI_BLACK));
  tui_damage();
  // Clear the real screen and hide the real cursor; from here on the
  // grid is the only thing that decides what is on it.
  printf("\033[2J\033[?25l\033[H");
}

void tui_hwcursor (int on) { tui_hwcur = on; printf(on ? "\033[?25h" : "\033[?25l"); }

void tui_end () {
  printf("\033[0m\033[?25h\033[%d;1H\n", tui_h);
  if (tui_fd >= 0) close(tui_fd);
}

// ---- widgets -------------------------------------------------------
// Each one draws into the grid and returns; the modal ones run their own
// key loop and put back what they covered. None of them owns the screen,
// so an editor can mix them with its own drawing freely.

// A menu BAR: names laid out left to right, one highlighted. The caller
// owns the array of names and the index; this only draws.
void tui_menubar (int y, int *names, int n, int sel, int a, int asel) {
  int i, x;
  tui_fill(0, y, tui_w, 1, ' ', a);
  x = 1;
  i = 0;
  while (i < n) {
    if (i == sel) {
      tui_put(x - 1, y, ' ', asel);
      x = tui_text(x, y, (char *)names[i], asel);
      tui_put(x, y, ' ', asel); ++x;
    } else {
      x = tui_text(x, y, (char *)names[i], a);
      x = x + 2;
    }
    ++i;
  }
}

// Where a menu bar's nth name starts, so a pull-down can line up under
// it. Same walk as above, which is why it is a separate word rather
// than a guess at the caller's end.
int tui_menux (int *names, int n, int which) {
  int i, x;
  x = 1; i = 0;
  while (i < which && i < n) { x = x + tui_strlen((char *)names[i]) + 2; ++i; }
  return x - 1;
}

// A pull-down or pop-up list. Runs its own loop and returns the chosen
// index, or -1 for Escape. The caller's screen is saved and restored,
// so nothing has to be redrawn afterwards.
//
// Items beginning with '-' are separators and cannot be selected, which
// is how a DOS menu groups things.
int tui_popup (int x, int y, int *items, int n, int start, int a, int asel) {
  int w, h, i, k, sel, blk, len;
  w = 0; i = 0;
  while (i < n) { len = tui_strlen((char *)items[i]); if (len > w) w = len; ++i; }
  w = w + 4; h = n + 2;
  if (x + w > tui_w) x = tui_w - w;
  if (x < 0) x = 0;
  if (y + h > tui_h) y = tui_h - h;
  if (y < 0) y = 0;
  blk = tui_save(x, y, w + 1, h + 1);
  sel = start;
  if (sel < 0 || sel >= n) sel = 0;
  while (*(char *)items[sel] == '-') { ++sel; if (sel >= n) sel = 0; }

  while (1) {
    tui_window(x, y, w, h, 0, a);
    tui_shadow(x, y, w, h);
    i = 0;
    while (i < n) {
      if (*(char *)items[i] == '-')
        tui_fill(x + 1, y + 1 + i, w - 2, 1, '-', a);
      else
        tui_textf(x + 2, y + 1 + i, (char *)items[i], i == sel ? asel : a, w - 4);
      ++i;
    }
    tui_flush();
    k = tui_key();
    if (k == TUI_ESC)   { tui_restore(blk); return 0 - 1; }
    if (k == TUI_ENTER) { tui_restore(blk); return sel; }
    if (k == TUI_UP || k == TUI_DOWN) {
      i = k == TUI_UP ? 0 - 1 : 1;
      while (1) {
        sel = sel + i;
        if (sel < 0) sel = n - 1;
        if (sel >= n) sel = 0;
        if (*(char *)items[sel] != '-') break;
      }
    }
    if (k == TUI_HOME) { sel = 0; while (*(char *)items[sel] == '-') ++sel; }
    if (k == TUI_END)  { sel = n - 1; while (*(char *)items[sel] == '-') --sel; }
  }
}

// A message with buttons along the bottom. Returns the button index, or
// -1 for Escape. buttons may be 0, in which case any key dismisses it --
// which is what an error box wants.
int tui_dialog (char *title, char *msg, int *buttons, int nb, int a, int asel) {
  int w, h, x, y, i, k, sel, blk, bx, bw;
  w = tui_strlen(msg) + 6;
  if (title) { i = tui_strlen(title) + 6; if (i > w) w = i; }
  bw = 0; i = 0;
  while (i < nb) { bw = bw + tui_strlen((char *)buttons[i]) + 4; ++i; }
  if (bw + 4 > w) w = bw + 4;
  if (w > tui_w - 2) w = tui_w - 2;
  h = nb > 0 ? 6 : 5;
  x = (tui_w - w) / 2; y = (tui_h - h) / 2;
  blk = tui_save(x, y, w + 1, h + 1);
  sel = 0;
  while (1) {
    tui_window(x, y, w, h, title, a);
    tui_shadow(x, y, w, h);
    tui_text(x + 3, y + 2, msg, a);
    if (nb > 0) {
      bx = x + (w - bw) / 2;
      i = 0;
      while (i < nb) {
        bx = tui_text(bx, y + 4, "[ ", i == sel ? asel : a);
        bx = tui_text(bx, y + 4, (char *)buttons[i], i == sel ? asel : a);
        bx = tui_text(bx, y + 4, " ]", i == sel ? asel : a);
        ++i;
      }
    }
    tui_flush();
    k = tui_key();
    if (nb == 0) { tui_restore(blk); return 0; }
    if (k == TUI_ESC)   { tui_restore(blk); return 0 - 1; }
    if (k == TUI_ENTER) { tui_restore(blk); return sel; }
    if (k == TUI_LEFT)  { --sel; if (sel < 0) sel = nb - 1; }
    if (k == TUI_RIGHT || k == TUI_TAB) { ++sel; if (sel >= nb) sel = 0; }
  }
}

// A single-line editable field, in place, with a drawn cursor. Returns
// 1 on Enter and 0 on Escape; buf is left as it was on Escape, so a
// caller can offer a default without copying it first.
int tui_input (int x, int y, int w, char *buf, int max, int a) {
  int len, pos, k, i, off, blk;
  char *tmp;
  tmp = malloc(max + 1);
  if (!tmp) return 0;
  len = tui_strlen(buf);
  i = 0; while (i <= len) { tmp[i] = buf[i]; ++i; }
  pos = len; off = 0;
  blk = tui_save(x, y, w, 1);
  while (1) {
    if (pos < off) off = pos;
    if (pos - off >= w) off = pos - w + 1;
    i = 0;
    while (i < w) { tui_put(x + i, y, off + i < len ? tmp[off + i] : ' ', a); ++i; }
    tui_cursor(x + pos - off, y);
    tui_flush();
    k = tui_key();
    if (k == TUI_ENTER) {
      i = 0; while (i <= len) { buf[i] = tmp[i]; ++i; }
      buf[len] = 0;
      free(tmp); tui_restore(blk); tui_nocursor(); return 1;
    }
    if (k == TUI_ESC) { free(tmp); tui_restore(blk); tui_nocursor(); return 0; }
    if (k == TUI_LEFT)  { if (pos > 0) --pos; }
    else if (k == TUI_RIGHT) { if (pos < len) ++pos; }
    else if (k == TUI_HOME)  { pos = 0; }
    else if (k == TUI_END)   { pos = len; }
    else if (k == 8 || k == TUI_BS) {
      if (pos > 0) { i = pos; while (i <= len) { tmp[i - 1] = tmp[i]; ++i; } --pos; --len; }
    }
    else if (k == TUI_DEL) {
      if (pos < len) { i = pos; while (i < len) { tmp[i] = tmp[i + 1]; ++i; } --len; }
    }
    else if (k >= 32 && k < 127 && len < max) {
      i = len; while (i >= pos) { tmp[i + 1] = tmp[i]; --i; }
      tmp[pos] = k; ++pos; ++len;
    }
  }
}

// A scrolling picker: the shape a file-open box and a "find in files"
// result list both want. Returns the chosen index, or -1.
int tui_list (int x, int y, int w, int h, char *title, int *items, int n,
              int start, int a, int asel) {
  int sel, top, i, k, rows, blk;
  rows = h - 2;
  sel = start; if (sel < 0 || sel >= n) sel = 0;
  top = 0;
  blk = tui_save(x, y, w + 1, h + 1);
  while (1) {
    if (sel < top) top = sel;
    if (sel >= top + rows) top = sel - rows + 1;
    tui_window(x, y, w, h, title, a);
    tui_shadow(x, y, w, h);
    i = 0;
    while (i < rows) {
      if (top + i < n)
        tui_textf(x + 1, y + 1 + i, (char *)items[top + i],
                  top + i == sel ? asel : a, w - 2);
      ++i;
    }
    // A scroll bar, when there is more than fits -- drawn in the frame
    // itself, which is where DOS put it.
    if (n > rows) {
      i = (sel * (rows - 1)) / (n - 1);
      tui_put(x + w - 1, y + 1 + i, '#', a);
    }
    tui_flush();
    k = tui_key();
    if (k == TUI_ESC)   { tui_restore(blk); return 0 - 1; }
    if (k == TUI_ENTER) { tui_restore(blk); return sel; }
    if (k == TUI_UP)    { if (sel > 0) --sel; }
    if (k == TUI_DOWN)  { if (sel < n - 1) ++sel; }
    if (k == TUI_PGUP)  { sel = sel - rows; if (sel < 0) sel = 0; }
    if (k == TUI_PGDN)  { sel = sel + rows; if (sel >= n) sel = n - 1; }
    if (k == TUI_HOME)  { sel = 0; }
    if (k == TUI_END)   { sel = n - 1; }
  }
}

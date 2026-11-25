// textgrid.h -- a VT100 text grid for a window (textgrid.c): what the C4IX
// desktop's Terminal draws, and grin's DOS Prompt will. No pipes: the
// window's code feeds it output and sends on the lines it edits.

enum { TG_COLS = 80, TG_ROWS = 25, TG_CW = 8, TG_CH = 16, TG_FONT = 14, TG_LINE = 250,
       TG_RUNMAX = 24, TG_HIST = 16, TG_HIST_W = 256, TG_SB_ROWS = 300, TG_SB_W = 16,
       TG_APC = 300 };
// the grid's size on the screen, inside its sunken edge, and with its scroll bar
enum { TG_W = TG_COLS * TG_CW + 4, TG_H = TG_ROWS * TG_CH + 4 };

struct tgrid {
    int win;                          // the window it is drawn in
    int cx, cy, wrap, fg, bg, bold, rev, cursor;
    int esc, np, param[8], priv;
    int alen;
    char abuf[TG_APC + 4];            // an ESC _ ... ESC \ command being read
    void (*apc)(char *ctx, char *s, int n);   // what one is for (0: dropped)
    char *ctx;
    int cell[TG_ROWS * TG_COLS];      // ch | fg << 8 | bg << 16, xterm-256 indices
    // The row cache (row_build): each row's background rectangles and
    // runs of text, as [first column, columns, kind << 8 | colour] with
    // kind 0 a rectangle and 1 text. rowok[r] is 0 until it is built, and
    // again whenever a cell of the row changes; rowk[r] is its run count,
    // or -1 for a row with more than TG_RUNMAX, drawn from the cells.
    int rowok[TG_ROWS], rowk[TG_ROWS];
    int runs[TG_ROWS * TG_RUNMAX * 3];
    // Line editing (tg_key): the line being typed, the cursor's place in
    // it, and where on the grid it starts (erow may go above the top as
    // the line scrolls). The history: hn lines, oldest first; hpos is the
    // one shown, hn for the line being typed, kept in saved meanwhile.
    int llen, lpos, erow, ecol;
    char line[TG_HIST_W];
    int hn, hpos, slen;
    char hist[TG_HIST * TG_HIST_W];
    char saved[TG_HIST_W];
    // The scrollback: a ring of rows that scrolled off the top, sbn of
    // them, the next going in at sbhead; view is how many rows back the
    // window shows (0, the live grid).
    int *sb;
    int sbn, sbhead, view;
    // A selection (tg_select): from a line and column to a line and column,
    // the lines numbered from the grid's first (nscr: the rows scrolled off
    // so far), so it stays on its text as more arrives. sel is 0 for none,
    // 1 a run of lines, 2 a block.
    int nscr, sel, sl0, sc0, sl1, sc1, selecting;
};

extern int tg_pal[256];               // xterm's 256 colours

struct tgrid *tg_new(int win);        // a cleared grid with its scrollback, or 0 for no memory
void tg_free(struct tgrid *g);
void tg_feed(struct tgrid *g, char *buf, int n);   // output: text and VT100 sequences
void tg_say(struct tgrid *g, char *s);
void tg_echo(struct tgrid *g, int c);
int  tg_key(struct tgrid *g, int code, int ch, int mods);
        // a key typed: the line editor, and PgUp/PgDn for the scrollback.
        // The length of a line Enter finished (with its newline, in
        // g->line), or 0
void tg_interrupt(struct tgrid *g);   // ^C: the line typed so far dropped
void tg_clear_screen(struct tgrid *g);   // Ctrl+L
void tg_scroll(struct tgrid *g, int rows);   // the scrollback: rows further back (negative: forward)
void tg_draw(struct tgrid *g, int x, int y, int focused);   // the grid at x, y and its scroll bar
void tg_mouse(struct tgrid *g, int ev, int x, int y);       // its scroll bar (E_WHEEL: y the steps)
// The selection, made with the mouse on the grid (x, y inside its sunken
// edge): a press starts one, a drag takes it on, and with block it is a
// rectangle rather than a run of lines. A click with no drag drops it.
void tg_select(struct tgrid *g, int ev, int x, int y, int block);
void tg_select_all(struct tgrid *g);  // the scrollback and the screen
void tg_unselect(struct tgrid *g);
int  tg_copy(struct tgrid *g, int all);   // the selection (or, all, everything) onto the
                                          // clipboard, a line a row; 0 if nothing was

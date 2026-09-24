// window.h -- a window of your own, for a program run in the desktop.
// docs/c4ix-desktop.md, part three.
//
// Run from a Command Prompt, a program that calls win_open() turns that
// window into its canvas, WIN_W by WIN_H pixels: it draws with the calls
// below and reads the mouse and keyboard with win_poll(). Everything
// travels through the program's own stdout and stdin, as escape
// sequences the Command Prompt understands, so there is nothing to
// attach to and nothing to clean up: when the program ends, the window
// is a terminal again.
//
// Built by either compiler. c4cc has no preprocessor, so give it this
// file ahead of the program:
//
//   c4cc -o /ram/demo.c4r /usr/include/window.h /usr/src/examples/bounce.c
//   /ram/demo.c4r
//
// and c4lc programs may #include "window.h". The subset is c4cc's: no
// structs, no global arrays, no #define; the only way out is the C4IX
// system call gate, the custom opcodes 200 and up.
//
// The wire format, for anyone writing another client:
//   program -> window   ESC _ G <op> <n>,<n>,... [;<text>] ESC \
//     o;title   become a canvas        c rgb             clear
//     r x,y,w,h,rgb  fill a rectangle  R x,y,w,h,rgb     outline one
//     l x0,y0,x1,y1,rgb  a line        d x,y,r,rgb       a filled disc
//     t x,y,rgb,size;text  text        p                 show the frame
//     q         be a terminal again
//   window -> program   ESC E <kind> <a>,<b>,<c> newline
//     s w,h,0 the canvas size   d x,y,button  u x,y,button
//     m x,y,buttons             k keycode,char,mods

enum {
    WIN_W = 640, WIN_H = 400,
    // win_poll's answers
    WIN_NONE = 0, WIN_DOWN = 1, WIN_UP = 2, WIN_MOVE = 3, WIN_KEY = 4, WIN_SIZE = 5,
    // key codes (browser keyCodes) and modifiers
    WIN_K_ESC = 27, WIN_K_ENTER = 13, WIN_K_LEFT = 37, WIN_K_UP = 38,
    WIN_K_RIGHT = 39, WIN_K_DOWN = 40, WIN_SHIFT = 1, WIN_CTRL = 2, WIN_ALT = 4,
    WIN_OUTCAP = 8192, WIN_INCAP = 1024
};

char *win_out;   // commands waiting to go out
int   win_olen;
char *win_in;    // event bytes read but not yet parsed
int   win_ilen;

// The C4IX system call gate (c4ix_user.h's SYS_* numbers).
int win_sys(int num, int a, int b, int c) { return __c4_opcode(c, b, a, num); }

void win_flush() {
    if (win_olen > 0) win_sys(200, 1, (int)win_out, win_olen);      // SYS_WRITE to stdout
    win_olen = 0;
}

void win_ch(int c) {
    if (win_olen >= WIN_OUTCAP) win_flush();
    win_out[win_olen] = c;
    win_olen = win_olen + 1;
}

void win_str(char *s) { while (*s) { win_ch(*s); s = s + 1; } }

void win_num(int v) {
    if (v < 0) { win_ch('-'); v = 0 - v; }
    if (v >= 10) win_num(v / 10);
    win_ch('0' + v % 10);
}

void win_begin(int op) { win_ch(27); win_ch('_'); win_ch('G'); win_ch(op); }
void win_end() { win_ch(27); win_ch('\\'); }
void win_arg(int v, int more) { win_num(v); if (more) win_ch(','); }

// Become a canvas, titled. 1 on success.
int win_open(char *title) {
    if (!win_out) {
        win_out = (char *)malloc(WIN_OUTCAP + 16);
        win_in = (char *)malloc(WIN_INCAP + 16);
        if (!win_out || !win_in) return 0;
    }
    win_olen = 0; win_ilen = 0;
    win_begin('o'); win_ch(';'); win_str(title); win_end();
    win_flush();
    return 1;
}

// Back to a terminal. A program that ends without this is fine too.
void win_close() { win_begin('q'); win_end(); win_flush(); }

void win_clear(int rgb) { win_begin('c'); win_arg(rgb, 0); win_end(); }
void win_rect(int x, int y, int w, int h, int rgb) {
    win_begin('r'); win_arg(x, 1); win_arg(y, 1); win_arg(w, 1); win_arg(h, 1); win_arg(rgb, 0); win_end();
}
void win_frame(int x, int y, int w, int h, int rgb) {
    win_begin('R'); win_arg(x, 1); win_arg(y, 1); win_arg(w, 1); win_arg(h, 1); win_arg(rgb, 0); win_end();
}
void win_line(int x0, int y0, int x1, int y1, int rgb) {
    win_begin('l'); win_arg(x0, 1); win_arg(y0, 1); win_arg(x1, 1); win_arg(y1, 1); win_arg(rgb, 0); win_end();
}
void win_disc(int x, int y, int r, int rgb) {
    win_begin('d'); win_arg(x, 1); win_arg(y, 1); win_arg(r, 1); win_arg(rgb, 0); win_end();
}
// size is the font's pixel height (11 is the desktop's own).
void win_text(int x, int y, int rgb, int size, char *s) {
    win_begin('t'); win_arg(x, 1); win_arg(y, 1); win_arg(rgb, 1); win_arg(size, 0);
    win_ch(';'); win_str(s); win_end();
}
// Show what was drawn since the last present, all at once.
void win_present() { win_begin('p'); win_end(); win_flush(); }

// Sleep, letting the rest of the machine run.
void win_sleep(int ms) { win_sys(220, ms, 0, 0); }                  // SYS_SLEEP

// The next event, or WIN_NONE; never waits. ev[0..2] get its numbers:
// x, y and the button for the mouse; keycode, character and modifiers
// for a key. -1 when stdin has ended (the window was closed).
int win_poll(int *ev) {
    int n, i, j, k, kind, v, neg;
    // take what has arrived
    n = win_sys(221, 0, 0, 0);                                        // SYS_AVAIL on stdin
    if (n < 0 && win_ilen == 0) return 0 - 1;
    if (n > WIN_INCAP - win_ilen) n = WIN_INCAP - win_ilen;
    if (n > 0) {
        n = win_sys(201, 0, (int)(win_in + win_ilen), n);            // SYS_READ
        if (n > 0) win_ilen = win_ilen + n;
    }
    // one whole line, if there is one
    i = 0;
    while (i < win_ilen && win_in[i] != 10) i = i + 1;
    if (i >= win_ilen) {
        if (win_ilen >= WIN_INCAP) win_ilen = 0;    // garbage without an end: drop it
        return WIN_NONE;
    }
    kind = WIN_NONE;
    if (i >= 3 && win_in[0] == 27 && win_in[1] == 'E') {
        k = win_in[2];
        if (k == 'd') kind = WIN_DOWN;
        else if (k == 'u') kind = WIN_UP;
        else if (k == 'm') kind = WIN_MOVE;
        else if (k == 'k') kind = WIN_KEY;
        else if (k == 's') kind = WIN_SIZE;
        j = 3; k = 0;
        while (k < 3) {
            v = 0; neg = 0;
            if (j < i && win_in[j] == '-') { neg = 1; j = j + 1; }
            while (j < i && win_in[j] >= '0' && win_in[j] <= '9') { v = v * 10 + (win_in[j] - '0'); j = j + 1; }
            if (neg) v = 0 - v;
            ev[k] = v;
            k = k + 1;
            if (j < i && win_in[j] == ',') j = j + 1;
        }
    }
    // drop the line
    i = i + 1;
    j = 0;
    while (i < win_ilen) { win_in[j] = win_in[i]; i = i + 1; j = j + 1; }
    win_ilen = j;
    return kind;
}

// window.h -- a window of your own, for a program run in the desktop.
// docs/c4ix-desktop.md, part three.
//
// Run from a Terminal, a program that calls win_open() turns that
// window into its canvas, WIN_W by WIN_H pixels: it draws with the calls
// below and reads the mouse and keyboard with win_poll(). Everything
// travels through the program's own stdout and stdin, as escape
// sequences the Terminal understands, so there is nothing to
// attach to and nothing to clean up: when the program ends, the window
// is a terminal again.
//
// Built by either compiler. The functions are in window.c, beside this
// file, which this includes unless C4IX_SHARED is defined: then a program
// links C4IX's shared libc4ix (c4rlink -l /lib/libc4ix.c4s), which has
// them, and carries no copy of its own. c4cc has no preprocessor, so give
// it both files ahead of the program:
//
//   c4cc -o /tmp/demo.c4r /usr/include/window.h /usr/include/window.c /usr/src/c4ix/examples/bounce.c
//   /tmp/demo.c4r
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
//     f addr,w,h,pitch,pal  an 8-bit framebuffer, scaled to the canvas
//     q         be a terminal again
//   window -> program   ESC E <kind> <a>,<b>,<c> newline
//     s w,h,0 the canvas size   d x,y,button  u x,y,button
//     m x,y,buttons             k keycode,char,mods
//     K keycode,char,mods (let go)  f 0,0,0 (a framebuffer frame was drawn)

#ifndef __C4IX_WINDOW_H
#define __C4IX_WINDOW_H 1

enum {
    WIN_W = 640, WIN_H = 400,
    // win_poll's answers
    WIN_NONE = 0, WIN_DOWN = 1, WIN_UP = 2, WIN_MOVE = 3, WIN_KEY = 4, WIN_SIZE = 5,
    WIN_KEYUP = 6, WIN_FRAME = 7,
    // key codes (browser keyCodes) and modifiers
    WIN_K_ESC = 27, WIN_K_ENTER = 13, WIN_K_LEFT = 37, WIN_K_UP = 38,
    WIN_K_RIGHT = 39, WIN_K_DOWN = 40, WIN_SHIFT = 1, WIN_CTRL = 2, WIN_ALT = 4,
    WIN_OUTCAP = 8192, WIN_INCAP = 1024
};

// the functions (window.c)
int win_sys(int num, int a, int b, int c);
void win_flush();
void win_ch(int c);
void win_str(char *s);
void win_num(int v);
void win_begin(int op);
void win_end();
void win_arg(int v, int more);
int win_open(char *title);
void win_close();
void win_clear(int rgb);
void win_rect(int x, int y, int w, int h, int rgb);
void win_frame(int x, int y, int w, int h, int rgb);
void win_line(int x0, int y0, int x1, int y1, int rgb);
void win_disc(int x, int y, int r, int rgb);
void win_text(int x, int y, int rgb, int size, char *s);
void win_present();
void win_fb(char *pixels, int w, int h, int pitch, int *pal);
void win_sleep(int ms);
int win_poll(int *ev);

#ifndef C4IX_SHARED
#include "window.c"
#endif

#endif // __C4IX_WINDOW_H

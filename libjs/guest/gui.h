// gui.h -- the libjs display, from inside the machine.
//
// A guest draws by writing command frames into a ring in its own memory
// and ringing a bell; mouse and key events come back through a second
// ring. Both are c4bb's mailbox format (include/c4bb_mbox.h): one region
// of the guest's choosing, events to the guest in the first half,
// commands to the host in the second. docs/libjs-design.md "The display
// device".
//
// ANNOUNCED, NEVER PROBED: call gui_present() first. On any machine
// without the display (native c4m, c4bb) the registers at 0x400 are
// ordinary memory, so a program must say "not fitted" and stop there.
//
// The functions are in gui.c, beside this file, which this includes
// unless C4IX_SHARED is defined: then a program links C4IX's shared
// library, /lib/libgui.c4s, instead (c4rlink -l), and carries no copy of
// its own. c4cc has no #include: pass this file and gui.c as sources ahead
// of the program, as build-images.sh does. Written in the subset both
// compilers accept: locals at the top of each function, literal enum
// values.
//
//   int *region;
//   if (!gui_present()) { printf("not fitted\n"); return 0; }
//   region = malloc(65536);
//   gui_attach(region, 65536, 640, 480);
//   gui_clear(0x000000); gui_rect(10, 10, 50, 50, 0xff2d95);
//   gui_text(10, 70, 0xffffff, 16, "hello"); gui_show();
//   while (gui_poll(ev)) ...

#ifndef __C4_GUI_H
#define __C4_GUI_H 1

enum {
	GUI_CAPS = 0x400, GUI_W = 0x404, GUI_H = 0x408, GUI_RINGLEN = 0x40c, GUI_RING = 0x410,
	GUI_BELL = 0x414, GUI_EVMASK = 0x418, GUI_IRQ = 0x41c, GUI_MOUSE = 0x420,
	GUI_BUTTONS = 0x424, GUI_TICKS = 0x428, GUI_DROPPED = 0x42c,
	GUI_FB = 0x430, GUI_FBPITCH = 0x434, GUI_FBFLIP = 0x438, GUI_FBMODE = 0x43c, GUI_CLOCK = 0x440,
	C4I_GUI = 0x4000
};
// commands
enum {
	GUI_CLEAR = 1, GUI_RECT = 2, GUI_RECTO = 3, GUI_LINE = 4, GUI_CIRCLE = 5, GUI_TEXT = 6,
	GUI_PIXEL = 7, GUI_PRESENT = 8, GUI_SIZE = 9, GUI_IMGDEF = 10, GUI_IMG = 11,
	GUI_CLIP = 12, GUI_NOCLIP = 13, GUI_TEXT2 = 14, GUI_CURSOR = 15, GUI_BLIT8 = 16
};
// pointers for gui_cursor: the host's own, so moving it draws nothing
enum { GUI_PTR_ARROW = 0, GUI_PTR_TEXT = 1, GUI_PTR_RESIZE = 2, GUI_PTR_HAND = 3,
       GUI_PTR_MOVE = 4, GUI_PTR_WAIT = 5, GUI_PTR_CROSS = 6, GUI_PTR_NONE = 7 };
enum { GUI_MONO = 0, GUI_SANS = 1, GUI_SANS_BOLD = 2 };
// events: ev[0] is the type, ev[1..3] its payload
enum {
	GUI_EV_MOVE = 1,     // x, y, buttons
	GUI_EV_DOWN = 2,     // x, y, button (0 left, 1 middle, 2 right)
	GUI_EV_UP = 3,       // x, y, button
	GUI_EV_KEYDOWN = 4,  // keycode, character (0 if none), modifiers (1 shift 2 ctrl 4 alt)
	GUI_EV_KEYUP = 5,    // keycode, character, modifiers
	GUI_EV_RESIZE = 6,   // width, height
	GUI_EV_FOCUS = 7,    // 1 gained, 0 lost
	GUI_EV_WHEEL = 8     // x, y, direction
};
// event mask bits for gui_events()
enum { GUI_M_MOVE = 1, GUI_M_BUTTONS = 2, GUI_M_KEYS = 4, GUI_M_WINDOW = 8, GUI_M_WHEEL = 16 };

// how gui_irq raises HARD_IRQ(3), and the functions (gui.c)
enum { HIRQ_GUI = 3 };
int gui_present ();
int gui_attach (int *region, int bytes, int w, int h);
void gui_detach ();
void gui_events (int mask);
void gui_flush ();
int *gui_frame (int type, int n);
void gui_commit ();
void gui_cmd0 (int type);
void gui_cmd3 (int type, int a, int b, int c);
void gui_cmd5 (int type, int a, int b, int c, int d, int e);
void gui_clear (int rgb);
void gui_cursor (int shape);
void gui_rect (int x, int y, int w, int h, int rgb);
void gui_recto (int x, int y, int w, int h, int rgb);
void gui_line (int x0, int y0, int x1, int y1, int rgb);
void gui_circle (int x, int y, int r, int rgb, int fill);
void gui_disc (int x, int y, int r, int rgb);
void gui_pixel (int x, int y, int rgb);
void gui_clip (int x, int y, int w, int h);
void gui_noclip ();
void gui_size (int w, int h);
void gui_text (int x, int y, int rgb, int size, char *s);
void gui_text2 (int x, int y, int rgb, int size, int font, int advance, char *s, int n);
void gui_imgdef (int id, int w, int h, int *pixels);
void gui_image (int id, int x, int y);
void gui_blit8 (int x, int y, int w, int h, char *src, int pitch, int *pal, int dw, int dh);
void gui_show ();
int gui_poll (int *ev);
void gui_fb (int *pixels, int pitch);
void gui_fb_columns (int *pixels);
void gui_flip (int w, int h);
void gui_irq (int on);
int gui_mouse_x ();
int gui_mouse_y ();
int gui_buttons ();
int gui_ticks ();
int gui_clock ();

// the size the display gave, and the events ring (for uevwait)
int  gui_width ();
int  gui_height ();
int *gui_events_ring ();

#ifndef C4IX_SHARED
#include "gui.c"
#endif

#endif // __C4_GUI_H

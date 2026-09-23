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
// c4cc has no #include: pass this file as a source ahead of the program,
// as build-images.sh does for c4bb_mbox.h. c4lc can #include it. Written
// in the subset both accept: locals at the top of each function, literal
// enum values.
//
//   int *region;
//   if (!gui_present()) { printf("not fitted\n"); return 0; }
//   region = malloc(65536);
//   gui_attach(region, 65536, 640, 480);
//   gui_clear(0x000000); gui_rect(10, 10, 50, 50, 0xff2d95);
//   gui_text(10, 70, 0xffffff, 16, "hello"); gui_show();
//   while (gui_poll(ev)) ...

enum {
	GUI_CAPS = 0x400, GUI_W = 0x404, GUI_H = 0x408, GUI_RINGLEN = 0x40c, GUI_RING = 0x410,
	GUI_BELL = 0x414, GUI_EVMASK = 0x418, GUI_IRQ = 0x41c, GUI_MOUSE = 0x420,
	GUI_BUTTONS = 0x424, GUI_TICKS = 0x428, GUI_DROPPED = 0x42c,
	C4I_GUI = 0x4000
};
// commands
enum {
	GUI_CLEAR = 1, GUI_RECT = 2, GUI_RECTO = 3, GUI_LINE = 4, GUI_CIRCLE = 5, GUI_TEXT = 6,
	GUI_PIXEL = 7, GUI_PRESENT = 8, GUI_SIZE = 9, GUI_IMGDEF = 10, GUI_IMG = 11,
	GUI_CLIP = 12, GUI_NOCLIP = 13
};
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

int *gui_in;     // events: the host writes, we read
int *gui_out;    // commands: we write, the host reads
int *gui_f;      // the frame being written
int  gui_flen;
int  gui_seq;
int  gui_w;
int  gui_h;

int gui_present () {
	return (__c4_info() & C4I_GUI) != 0;
}

// Hand the display a region of our memory for its rings and ask for a
// size. bytes should be a multiple of 8 and at least 1024. 1 on success.
int gui_attach (int *region, int bytes, int w, int h) {
	int half;
	if (!gui_present()) return 0;
	bytes = bytes & ~7;
	half = bytes / 2;
	*(int *)GUI_W = w;
	*(int *)GUI_H = h;
	*(int *)GUI_RINGLEN = bytes;
	*(int *)GUI_RING = (int)region;
	if (*(int *)GUI_RING != (int)region) return 0;
	gui_in = region;
	gui_out = (int *)((int)region + half);
	gui_seq = 0;
	gui_w = *(int *)GUI_W;
	gui_h = *(int *)GUI_H;
	return 1;
}

void gui_detach () { *(int *)GUI_RING = 0; }

// Which events to deliver (GUI_M_*). Moves are off until asked for.
void gui_events (int mask) { *(int *)GUI_EVMASK = mask; }

// Send what has been written so far. The host takes it on the spot.
void gui_flush () { *(int *)GUI_BELL = 1; }

// Room for a command with n payload words, or 0. A full ring is flushed
// and tried again: the host empties it inside the bell's store.
int *gui_frame (int type, int n) {
	int cap, head, tail, at, len, room, tries;
	len = 3 + n;
	cap = gui_out[0];
	if (len > cap) return 0;
	tries = 0;
	while (tries < 3) {
		head = gui_out[1]; tail = gui_out[2];
		room = cap - (head - tail);
		at = head % cap;
		if (at + len > cap && room >= (cap - at) + len) {
			gui_out[3 + at] = -1;              // skip to the start of the ring
			head = head + (cap - at);
			gui_out[1] = head;
			at = 0;
			room = cap - (head - tail);
		}
		if (at + len <= cap && room >= len) {
			gui_f = gui_out + 3 + at;
			gui_f[1] = type;
			gui_f[2] = gui_seq;
			gui_seq = gui_seq + 1;
			gui_flen = len;
			return gui_f + 3;
		}
		*(int *)GUI_BELL = 1;
		tries = tries + 1;
	}
	return 0;
}

// The frame is whole: its length goes in last, then the head moves.
void gui_commit () {
	gui_f[0] = gui_flen;
	gui_out[1] = gui_out[1] + gui_flen;
}

void gui_cmd0 (int type) {
	if (gui_frame(type, 0)) gui_commit();
}

void gui_cmd3 (int type, int a, int b, int c) {
	int *p;
	if (!(p = gui_frame(type, 3))) return;
	p[0] = a; p[1] = b; p[2] = c;
	gui_commit();
}

void gui_cmd5 (int type, int a, int b, int c, int d, int e) {
	int *p;
	if (!(p = gui_frame(type, 5))) return;
	p[0] = a; p[1] = b; p[2] = c; p[3] = d; p[4] = e;
	gui_commit();
}

void gui_clear  (int rgb)                             { int *p; if ((p = gui_frame(GUI_CLEAR, 1))) { p[0] = rgb; gui_commit(); } }
void gui_rect   (int x, int y, int w, int h, int rgb) { gui_cmd5(GUI_RECT, x, y, w, h, rgb); }
void gui_recto  (int x, int y, int w, int h, int rgb) { gui_cmd5(GUI_RECTO, x, y, w, h, rgb); }
void gui_line   (int x0, int y0, int x1, int y1, int rgb) { gui_cmd5(GUI_LINE, x0, y0, x1, y1, rgb); }
void gui_circle (int x, int y, int r, int rgb, int fill)  { gui_cmd5(GUI_CIRCLE, x, y, r, rgb, fill); }
void gui_disc   (int x, int y, int r, int rgb)        { gui_cmd5(GUI_CIRCLE, x, y, r, rgb, 1); }
void gui_pixel  (int x, int y, int rgb)               { gui_cmd3(GUI_PIXEL, x, y, rgb); }
void gui_clip   (int x, int y, int w, int h)          { int *p; if ((p = gui_frame(GUI_CLIP, 4))) { p[0] = x; p[1] = y; p[2] = w; p[3] = h; gui_commit(); } }
void gui_noclip ()                                    { gui_cmd0(GUI_NOCLIP); }
void gui_size   (int w, int h)                        { int *p; if ((p = gui_frame(GUI_SIZE, 2))) { p[0] = w; p[1] = h; gui_commit(); } }

// Text at (x, y), top-left, size in pixels. Characters travel four to a
// word, lowest byte first.
void gui_text (int x, int y, int rgb, int size, char *s) {
	int n, i, *p;
	n = 0;
	while (s[n]) n = n + 1;
	if (!(p = gui_frame(GUI_TEXT, 5 + (n + 3) / 4))) return;
	p[0] = x; p[1] = y; p[2] = rgb; p[3] = size; p[4] = n;
	i = 0;
	while (i < (n + 3) / 4) { p[5 + i] = 0; i = i + 1; }
	i = 0;
	while (i < n) {
		p[5 + i / 4] = p[5 + i / 4] | ((s[i] & 255) << ((i % 4) * 8));
		i = i + 1;
	}
	gui_commit();
}

// An image the host keeps under `id` for gui_image: w*h pixels of
// 0x00RRGGBB, where a top byte of 255 is fully transparent.
void gui_imgdef (int id, int w, int h, int *pixels) {
	int i, *p;
	if (!(p = gui_frame(GUI_IMGDEF, 3 + w * h))) return;
	p[0] = id; p[1] = w; p[2] = h;
	i = 0;
	while (i < w * h) { p[3 + i] = pixels[i]; i = i + 1; }
	gui_commit();
}
void gui_image (int id, int x, int y) { gui_cmd3(GUI_IMG, id, x, y); }

// Show the frame drawn so far, all at once, and send it.
void gui_show () {
	gui_cmd0(GUI_PRESENT);
	*(int *)GUI_BELL = 2;
}

// The next event into ev[0..3]; 1 if there was one.
int gui_poll (int *ev) {
	int cap, head, tail, at, *f, n, i;
	cap = gui_in[0]; head = gui_in[1]; tail = gui_in[2];
	if (head == tail) return 0;
	at = tail % cap;
	if (gui_in[3 + at] == -1) {
		tail = tail + (cap - at);
		gui_in[2] = tail;
		if (head == tail) return 0;
		at = 0;
	}
	f = gui_in + 3 + at;
	ev[0] = f[1];
	ev[1] = 0; ev[2] = 0; ev[3] = 0;
	n = f[0] - 3;
	if (n > 3) n = 3;
	i = 0;
	while (i < n) { ev[1 + i] = f[3 + i]; i = i + 1; }
	gui_in[2] = gui_in[2] + f[0];
	return 1;
}

int gui_mouse_x () { return *(int *)GUI_MOUSE >> 16; }
int gui_mouse_y () { return *(int *)GUI_MOUSE & 65535; }
int gui_buttons () { return *(int *)GUI_BUTTONS; }
int gui_ticks ()   { return *(int *)GUI_TICKS; }

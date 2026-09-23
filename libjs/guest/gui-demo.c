// gui-demo.c -- the display, driven from a bare c4m program.
//
// A box bounces, a ring follows the mouse, clicks leave marks, keys
// typed on the display are echoed to the terminal. q on the display
// quits. `gui-demo N` stops after N frames (the headless test uses it).
//
// On a machine without the display (native c4m, c4bb) it prints
// "gui: not fitted" and exits 0: the registers it would use are ordinary
// memory there, so it must not touch them.
//
// Built with libjs/guest/gui.h passed as a source ahead of this file:
//   ./c4cc32 -o gui-demo.c4r libjs/guest/gui.h libjs/guest/gui-demo.c

enum { W = 640, H = 480, RING = 65536, MARKS = 32 };

char numbuf[16];

// c4 has no sprintf: decimal into numbuf.
char *num (int v) {
	int i, neg, j;
	char t;
	i = 0; neg = 0;
	if (v < 0) { neg = 1; v = 0 - v; }
	while (1) { numbuf[i] = '0' + v % 10; i = i + 1; v = v / 10; if (!v) break; }
	if (neg) { numbuf[i] = '-'; i = i + 1; }
	numbuf[i] = 0;
	j = 0; i = i - 1;
	while (j < i) { t = numbuf[j]; numbuf[j] = numbuf[i]; numbuf[i] = t; j = j + 1; i = i - 1; }
	return numbuf;
}

int main (int argc, char **argv) {
	int *region, *ev, *mx, *my, x, y, dx, dy, frame, limit, running, marks, i, gx, mouse_x, mouse_y, keys, clicks, c;
	char *s;

	if (!gui_present()) { printf("gui: not fitted\n"); return 0; }
	region = malloc(RING);
	ev = malloc(4 * sizeof(int));
	mx = malloc(MARKS * sizeof(int));
	my = malloc(MARKS * sizeof(int));
	if (!gui_attach(region, RING, W, H)) { printf("gui: the display refused the ring\n"); return 1; }
	gui_events(GUI_M_MOVE | GUI_M_BUTTONS | GUI_M_KEYS | GUI_M_WINDOW);

	limit = 0;
	if (argc > 1) { s = argv[1]; while (*s) { limit = limit * 10 + (*s - '0'); s = s + 1; } }
	printf("gui: fitted, %dx%d. Click the display, move, type; q there quits.\n", gui_w, gui_h);

	x = 40; y = 60; dx = 3; dy = 2;
	frame = 0; running = 1; marks = 0; keys = 0; clicks = 0;
	mouse_x = W / 2; mouse_y = H / 2;
	while (running) {
		while (gui_poll(ev)) {
			if (ev[0] == GUI_EV_MOVE) { mouse_x = ev[1]; mouse_y = ev[2]; }
			else if (ev[0] == GUI_EV_DOWN) {
				mx[marks % MARKS] = ev[1]; my[marks % MARKS] = ev[2];
				marks = marks + 1; clicks = clicks + 1;
				printf("gui: click at %d,%d\n", ev[1], ev[2]);
			}
			else if (ev[0] == GUI_EV_KEYDOWN) {
				keys = keys + 1;
				c = ev[2];
				if (c >= 32 && c < 127) printf("gui: key %d '%c'\n", ev[1], c);
				else printf("gui: key %d\n", ev[1]);
				if (c == 'q') running = 0;
			}
		}

		x = x + dx; y = y + dy;
		if (x < 0 || x + 60 > gui_w) { dx = 0 - dx; x = x + dx + dx; }
		if (y < 40 || y + 60 > gui_h) { dy = 0 - dy; y = y + dy + dy; }

		gui_clear(0x0d0b1e);
		gx = 0;
		while (gx < gui_w) { gui_line(gx, 40, gx, gui_h, 0x1a1633); gx = gx + 40; }
		gx = 40;
		while (gx < gui_h) { gui_line(0, gx, gui_w, gx, 0x1a1633); gx = gx + 40; }
		gui_rect(0, 0, gui_w, 36, 0x14102a);
		gui_text(12, 10, 0xff2d95, 16, "c4m.js display");
		gui_text(200, 12, 0x8a83b6, 13, "frame");
		gui_text(250, 12, 0xc8c2e8, 13, num(frame));
		gui_text(330, 12, 0x8a83b6, 13, "keys");
		gui_text(372, 12, 0xc8c2e8, 13, num(keys));
		gui_text(420, 12, 0x8a83b6, 13, "clicks");
		gui_text(478, 12, 0xc8c2e8, 13, num(clicks));

		i = 0;
		while (i < marks && i < MARKS) { gui_disc(mx[i], my[i], 4, 0xffe66d); i = i + 1; }

		gui_rect(x, y, 60, 60, 0xff2d95);
		gui_recto(x - 3, y - 3, 66, 66, 0x00e5ff);
		gui_circle(mouse_x, mouse_y, 12, 0x3dff9e, 0);
		gui_text(mouse_x + 16, mouse_y - 6, 0x3dff9e, 14, "hello from c4m");
		gui_show();

		frame = frame + 1;
		if (limit && frame >= limit) running = 0;
		__c4_usleep(16000);
	}
	printf("gui: done after %d frames\n", frame);
	gui_detach();
	return 0;
}

// fb-demo.c -- the display's pixel framebuffer, from a bare c4m program.
//
// Every pixel of a 320x240 frame is computed by the guest and written to
// its own memory; gui_flip hands the frame over and the page scales it
// to the display. Text drawn with the command ring sits on top. Events
// arrive by interrupt instead of polling: gui_irq(1) and a cycle handler,
// the way a kernel would take them.
//
// `fb-demo N` stops after N frames; q on the display quits. On a machine
// without the display it prints "fb: not fitted" and exits.
//
//   ./c4cc32 -o fb-demo.c4r libjs/guest/gui.h libjs/guest/fb-demo.c

enum { FW = 320, FH = 240, RING = 32768 };
enum { TRAP_HARD_IRQ = 1, CONF_CYCLE_INTERRUPT_HANDLER = 1 };

int irqs;

void on_irq (int trap, int ins, int mode, int a, int *bp, int *sp, int *returnpc) {
	if (trap == TRAP_HARD_IRQ && ins == HIRQ_GUI) irqs = irqs + 1;
}

int main (int argc, char **argv) {
	int *region, *fb, *row, *ev, x, y, t, limit, running, frames, cx, cy, keys, seen;
	char *s;

	if (!gui_present()) { printf("fb: not fitted\n"); return 0; }
	region = malloc(RING);
	fb = malloc(FW * FH * sizeof(int));
	ev = malloc(4 * sizeof(int));
	if (!gui_attach(region, RING, 640, 480)) { printf("fb: the display refused the ring\n"); return 1; }
	gui_events(GUI_M_MOVE | GUI_M_BUTTONS | GUI_M_KEYS);
	gui_fb(fb, 0);
	__c4_configure(CONF_CYCLE_INTERRUPT_HANDLER, (int)&on_irq);
	gui_irq(1);

	limit = 0;
	if (argc > 1) { s = argv[1]; while (*s) { limit = limit * 10 + (*s - '0'); s = s + 1; } }
	printf("fb: %dx%d framebuffer, events by interrupt. q on the display quits.\n", FW, FH);

	t = 0; frames = 0; running = 1; keys = 0; seen = 0;
	cx = FW / 2; cy = FH / 2;
	while (running) {
		// Only look at the ring when an interrupt said something is there.
		if (irqs != seen) {
			seen = irqs;
			while (gui_poll(ev)) {
				if (ev[0] == GUI_EV_MOVE) { cx = ev[1] / 2; cy = ev[2] / 2; }
				else if (ev[0] == GUI_EV_KEYDOWN) {
					keys = keys + 1;
					if (ev[2] == 'q') running = 0;
				}
			}
		}
		y = 0;
		while (y < FH) {
			row = fb + y * FW;
			x = 0;
			while (x < FW) {
				row[x] = (((x + t) & 255) << 16) | (((y + y - t) & 255) << 8) | ((x ^ y) & 255);
				x = x + 1;
			}
			y = y + 1;
		}
		// a crosshair where the mouse is, in framebuffer pixels
		x = 0; while (x < FW) { fb[cy * FW + x] = 0xffffff; x = x + 1; }
		y = 0; while (y < FH) { fb[y * FW + cx] = 0xffffff; y = y + 1; }
		gui_flip(FW, FH);
		gui_text(12, 12, 0xffffff, 16, "c4m.js framebuffer");
		gui_show();

		t = t + 3; frames = frames + 1;
		if (limit && frames >= limit) running = 0;
		__c4_usleep(16000);
	}
	gui_irq(0);
	printf("fb: done after %d frames, %d event interrupts, %d keys\n", frames, irqs, keys);
	gui_detach();
	return 0;
}

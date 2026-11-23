// gui-wrap.c -- commands over half the display's command ring, each sent
// with the ring's head where one has to wrap and the wrap and the command
// together are more than the ring (libgui's gui_frame, c9681e6: such a
// command was dropped, and the desktop's picture with it). Every one must
// go. test-gui.mjs runs it and counts the IMGDEFs that reach the display.
//
// Built with libjs/guest/gui.h passed as a source ahead of this file:
//   ./c4cc32 -o gui-wrap.c4r libjs/guest/gui.h libjs/guest/gui.c libjs/guest/gui-wrap.c

enum { RING = 4096, IW = 20, IH = 15, N = 12 };

int px[300];

int main () {
	int *region, *p, k, sent, cap, len, moves;
	if (!gui_present()) { printf("gui-wrap: not fitted\n"); return 0; }
	region = (int *)malloc(RING);
	if (!gui_attach(region, RING, 320, 200)) { printf("gui-wrap: the ring was refused\n"); return 1; }
	k = 0;
	while (k < IW * IH) { px[k] = k * 4097; k = k + 1; }
	cap = gui_out[0];
	len = 3 + 3 + IW * IH;
	if (len * 2 <= cap) { printf("gui-wrap: the ring is too big for the test (%d words)\n", cap); return 1; }
	sent = 0; moves = 0;
	k = 0;
	while (k < N) {
		// small commands until the head is where this one cannot fit
		// before the end and the wrap and it are more than the ring
		while (!(gui_at > cap - len && gui_at < len)) { gui_rect(0, 0, 1, 1, k); moves = moves + 1; }
		if ((p = gui_frame(GUI_IMGDEF, 3 + IW * IH))) {
			p[0] = 100 + k; p[1] = IW; p[2] = IH;
			memcpy((char *)(p + 3), (char *)px, IW * IH * sizeof(int));
			gui_commit();
			sent = sent + 1;
		}
		k = k + 1;
	}
	gui_flush();
	printf("gui-wrap: %d of %d sent, the ring %d words, each %d\n", sent, N, cap, len);
	return sent == N ? 0 : 1;
}

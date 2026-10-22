// snd-demo.c -- the sound card (snd.h): a scale on the AdLib, a chord
// under its last note, and a sample over its first. Every note is
// timed by the stream, so the demo itself takes no time: it writes the
// whole tune at once and the card plays it out.
//
//   node libjs/cli.js --wav demo.wav src/c4bb/images/snd-demo.c4r
//
// or http://localhost:8472/libjs/web/?system=snd-demo after
// `make serve-libjs`. It waits for Enter first: a page may not make a
// sound until it has been used, and the key is that use.

// an OPL2 voice on channel ch: a soft organ
void voice (int ch) {
	int m, c;
	// the two operators of channels 0-2 are at 0,3 / 1,4 / 2,5
	m = ch;
	c = ch + 3;
	snd_opl(0x20 + m, 0x01); snd_opl(0x20 + c, 0x01);   // multiple 1
	snd_opl(0x40 + m, 0x10); snd_opl(0x40 + c, 0x00);   // modulator quieter
	snd_opl(0x60 + m, 0xf4); snd_opl(0x60 + c, 0xf4);   // fast attack, some decay
	snd_opl(0x80 + m, 0x55); snd_opl(0x80 + c, 0x56);   // sustain, release
	snd_opl(0xc0 + ch, 0x08);                            // feedback 4, FM
}

// note on: fnum and block are the OPL's (fnum 0x16b is C at block 4)
void note (int ch, int fnum, int block) {
	snd_opl(0xa0 + ch, fnum & 255);
	snd_opl(0xb0 + ch, 0x20 | (block << 2) | ((fnum >> 8) & 3));
}

void off (int ch, int fnum, int block) {
	snd_opl(0xb0 + ch, (block << 2) | ((fnum >> 8) & 3));
}

int main (int argc, char **argv) {
	int scale[8];
	char *saw, *key;
	int i;
	if (!snd_present()) { printf("snd-demo: no sound card\n"); return 1; }
	scale[0] = 0x16b; scale[1] = 0x181; scale[2] = 0x198; scale[3] = 0x1b0;
	scale[4] = 0x1ca; scale[5] = 0x1e5; scale[6] = 0x202; scale[7] = 0x221;
	snd_reset();
	snd_opl(0x01, 0x20);                     // waveform select on
	voice(0); voice(1); voice(2);
	snd_clock(1000);                          // snd_wait in milliseconds

	// a quarter second of a falling saw at 11025 Hz, over the first note
	saw = malloc(2756);
	i = 0;
	while (i < 2756) { saw[i] = 255 - ((i * 4) & 255); i = i + 1; }

	printf("snd-demo: Enter plays the tune\n");
	key = malloc(4);
	read(0, key, 1);
	snd_play(1, saw, 2756, 11025, 0x8080);

	i = 0;
	while (i < 8) {
		note(0, scale[i], 4);
		if (i == 7) { note(1, scale[2], 4); note(2, scale[4], 4); }
		snd_wait(i == 7 ? 600 : 180);
		off(0, scale[i], 4);
		snd_wait(20);
		i = i + 1;
	}
	off(1, scale[2], 4); off(2, scale[4], 4);
	snd_wait(800);                            // the release, then quiet
	printf("snd-demo: %d notes, %d us ahead\n", 8, snd_ahead());
	return 0;
}

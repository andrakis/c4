// snd.c -- the libjs sound card's functions (snd.h, which says how to use
// them). Included by snd.h, or built on its own into /lib/libgui.c4s.

#include "snd.h"

int snd_present () {
	return (__c4_info() & C4I_SND) != 0;
}

int snd_caps () {
	if (!snd_present()) return 0;
	return *(int *)SND_CAPS;
}

void snd_reset () { *(int *)SND_RESET = 1; }

void snd_opl (int reg, int val) { *(int *)SND_OPL = ((reg & 511) << 8) | (val & 255); }

void snd_clock (int units) { *(int *)SND_CLOCK = units; }

void snd_wait (int n) { *(int *)SND_WAIT = n; }

int snd_ahead () { return *(int *)SND_AHEAD; }

void snd_play (int ch, char *data, int len, int rate, int vol) {
	*(int *)SND_PCM_ADDR = (int)data;
	*(int *)SND_PCM_LEN = len;
	*(int *)SND_PCM_RATE = rate;
	*(int *)SND_PCM_VOL = vol;
	*(int *)SND_PCM_PLAY = ch;
}

void snd_stop (int ch) { *(int *)SND_PCM_STOP = ch; }

void snd_volume (int ch, int vol) {
	*(int *)SND_PCM_VOL = vol;
	*(int *)SND_PCM_SETVOL = ch;
}

int snd_busy () { return *(int *)SND_PCM_BUSY; }

void snd_speaker (int divisor) { *(int *)SND_SPK = divisor; }

void snd_master (int vol) { *(int *)SND_MASTER = vol; }

int snd_master_get () { return *(int *)SND_MASTER; }

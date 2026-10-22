// snd.h -- the libjs sound card, from inside the machine: an AdLib (OPL2,
// and the OPL3's second bank), eight channels of 8-bit samples, and the
// PC speaker. docs/libjs-design.md "The sound card".
//
// ANNOUNCED, NEVER PROBED: call snd_present() first. On a machine without
// the card the registers at 0x480 are ordinary memory.
//
// The OPL is written as a sound card's interrupt would write it: some
// registers, then snd_wait() for the time until the next ones. Each write
// is stamped with the stream's time, not with when the program got round
// to it, so music plays evenly however unevenly the program runs, as long
// as it keeps the stream ahead: make more while snd_ahead() is short.
// Samples and the speaker play now.
//
// The functions are in snd.c, beside this file, which this includes
// unless C4IX_SHARED is defined: then a program links /lib/libgui.c4s,
// which has them too. c4cc has no #include: pass this file and snd.c as
// sources ahead of the program, as build-images.sh does for snd-demo.c.
//
//   if (!snd_present()) { printf("no sound card\n"); return 0; }
//   snd_clock(1000);                       // snd_wait() in milliseconds
//   snd_opl(0x20, 0x01); ... snd_opl(0xb0, 0x31);    // a note on
//   snd_wait(500);
//   snd_opl(0xb0, 0x11);                   // and off, half a second later
//   snd_play(2, samples, n, 11025, 0xffff);

#ifndef __C4_SND_H
#define __C4_SND_H 1

enum {
	SND_CAPS = 0x480, SND_OPL = 0x484, SND_WAIT = 0x488, SND_AHEAD = 0x48c, SND_CLOCK = 0x490,
	SND_PCM_ADDR = 0x494, SND_PCM_LEN = 0x498, SND_PCM_RATE = 0x49c, SND_PCM_VOL = 0x4a0,
	SND_PCM_PLAY = 0x4a4, SND_PCM_STOP = 0x4a8, SND_PCM_SETVOL = 0x4ac, SND_PCM_BUSY = 0x4b0,
	SND_SPK = 0x4b4, SND_RESET = 0x4b8,
	C4I_SND = 0x8000
};
// CAPS
enum { SND_HAS_OPL = 1, SND_HAS_PCM = 2, SND_HAS_SPK = 4, SND_HAS_OPL3 = 8 };
enum { SND_CHANNELS = 8 };

int  snd_present ();
int  snd_caps ();
void snd_reset ();                         // every channel stopped, the OPL zeroed
// the AdLib
void snd_opl (int reg, int val);           // reg 0x00-0xff, 0x100-0x1ff the OPL3's second bank
void snd_clock (int units);                // snd_wait's units per second (default 1000000)
void snd_wait (int n);                     // the stream moves n units on
int  snd_ahead ();                         // microseconds the stream is ahead of now
// samples: unsigned 8-bit mono, copied when played; vol is left | (right << 8)
void snd_play (int ch, char *data, int len, int rate, int vol);
void snd_stop (int ch);                    // -1: all of them
void snd_volume (int ch, int vol);
int  snd_busy ();                          // a bit for each channel still playing
// the PC speaker: a square wave at 1193180 / divisor Hz, 0 for quiet
void snd_speaker (int divisor);

#ifndef C4IX_SHARED
#include "snd.c"
#endif

#endif // __C4_SND_H

/* snd.h -- the sound card (snd.c: libjs/snd-device.js and snd-mix.js) and
 * the OPL it mixes (opl.c: libjs/opl.js, DOSBox's dbopl). */
#ifndef C4PC_SND_H
#define C4PC_SND_H
#include "c4pc.h"
typedef int8_t i8;
typedef uint64_t u64;

typedef struct opl_chip opl_chip;
opl_chip *opl_new(u32 rate);
void opl_reset(opl_chip *c, u32 rate);
void opl_write(opl_chip *c, int reg, int val);
void opl_generate(opl_chip *c, u32 n, i32 *out);
int opl_stereo(opl_chip *c);
void opl_state(opl_chip *c, int *opl3, int *reg104, int *regBD);

/* the card: its registers at 0x480-0x4FF (bus.js), the machine's clock its
 * "now"; live (a sound card under it: pc/sb16.c), the mixer renders as the
 * card plays, `latency` frames after now; offline (c4pc-host --wav), every
 * event is kept and the whole session rendered at the end, as cli.js does */
typedef struct snd snd;
snd *snd_new(machine *m, u32 rate, int live);
i32 snd_read32(snd *s, u32 addr);
void snd_write32(snd *s, u32 addr, i32 v);
/* now, in microseconds of the card's clock */
double snd_now(snd *s);
/* live: n frames of 16-bit stereo, now's events placed latency frames on */
void snd_fill(snd *s, i16 *out, u32 n);
/* ... the card having AHEAD frames still to play before out's first: now
 * is placed by its playing (latency past it), the same whatever its lead */
void snd_fill_ahead(snd *s, i16 *out, u32 n, u32 ahead);
extern u32 snd_live_latency_ms;         /* a live card's: its lead + 20 ms (pc.c --snd-lead) */
/* the card and all it holds freed (a guest's, when it halts: pc/hv.c) */
void snd_free(snd *s);
/* since the last call: events that came after their time had been mixed,
 * the most late of them (ms), and the times the clocks were set again */
void snd_lateness(snd *s, u32 *n, u32 *most_ms, u32 *resyncs);
/* offline: the session as a WAV file's bytes (*len), to free */
u8 *snd_wav(snd *s, u32 *len);
/* "N OPL writes, M samples played" */
void snd_stats(snd *s, u32 *writes, u32 *plays);
#endif

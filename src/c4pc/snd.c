/* snd.c -- the sound card: libjs/snd-device.js (the registers, the events
 * they make, stamped with when they happen) and libjs/snd-mix.js (the
 * events made into samples: the AdLib through opl.c, sixteen channels of
 * 8-bit samples, the PC speaker), in C. No General MIDI synth: CAPS says
 * there is none, so a game plays its music on the AdLib, as on a machine
 * with no MPU-401.
 *
 * Registers, one word each, at 0x480-0x4FF (bus.js), as snd-device.js
 * lists them: CAPS, OPL, WAIT, AHEAD, CLOCK, PCM_ADDR, PCM_LEN, PCM_RATE,
 * PCM_VOL, PCM_PLAY, PCM_STOP, PCM_SETVOL, PCM_BUSY, SPK, RESET, MASTER,
 * MIDI, MIDI_DATA, PCM_START, PCM_CHVOL.
 *
 * Now is the machine's clock in microseconds: its cycles and sleeps, as
 * libjs's cli.js has it, so that a file rendered from a session (c4pc-host
 * --wav) is libjs's, sample for sample; or, at the keyboard (machine.wall),
 * the real clock, which is the one the card under it plays to.
 *
 * Offline every event is kept and the session rendered when it ends, as
 * renderWav does. Live, the card under the machine (pc/sb16.c) asks for
 * frames as it plays them, and the events of now go latency frames after
 * the last frame made, the offset set again whenever the two clocks have
 * drifted further apart than slack (snd-mix.js push). */
#include "snd.h"
#include "clib.h"

#define CHANNELS 16
#define PCM_MAX (4 << 20)
#define BLOCK 1024
#define OPL_GAIN (4.0 / 32768)       /* dbopl's output, scaled as Wolf4SDL scales it, to [-1, 1] */
#define PCM_GAIN (0.8 / 128)
#define SPK_AMP 0.15
#define PCM_STALE 0.2                /* seconds: a sound this late is dropped, not started */
#define HAS_START 64
#define MUTE 0x100

enum {
  CAPS = 0x480, OPL = 0x484, WAIT = 0x488, AHEAD = 0x48c, CLOCK = 0x490, PCM_ADDR = 0x494, PCM_LEN = 0x498,
  PCM_RATE = 0x49c, PCM_VOL = 0x4a0, PCM_PLAY = 0x4a4, PCM_STOP = 0x4a8, PCM_SETVOL = 0x4ac, PCM_BUSY = 0x4b0,
  SPK = 0x4b4, RESET = 0x4b8, MASTER = 0x4bc, MIDI = 0x4c0, MIDI_DATA = 0x4c4, PCM_START = 0x4c8, PCM_CHVOL = 0x4cc,
};
enum { EV_OPL = 1, EV_PCM, EV_STOP, EV_VOL, EV_SPK, EV_RESET, EV_MASTER };

typedef struct { int kind; double t; i32 a, b, c; u8 *bytes; u32 len; } evt;
typedef struct { u8 *data; u32 len; double pos, step, vl, vr; } voice;
struct snd {
  machine *m; int live;
  /* the device */
  i32 clock; double cursor;
  u32 pcm_addr; i32 pcm_len, pcm_rate, pcm_vol;
  i32 master; double ends[CHANNELS];
  u32 writes, plays;
  /* the mixer */
  u32 rate; opl_chip *opl; i32 obuf[2 * BLOCK];
  voice v[CHANNELS];
  double spk_freq, spk_phase, gain;
  evt *q; u32 qn, qcap, head;
  double offset; i64 frame, latency, slack; int synced; u32 resyncs;
  u32 late_n; i64 late_most;           /* events applied after their frame had been mixed: how many, the most late (frames) */
  float L[4096], R[4096];
};

double plat_now_us(void);
u32 dev_dma_read(machine *m, u32 addr, u32 len, u8 *out);

/* JavaScript's Math.floor, Math.ceil and Math.round, for numbers below 2^62 */
static double jfloor(double x) { double t = (double)(i64)x; return t > x ? t - 1 : t; }
static double jceil(double x) { double t = (double)(i64)x; return t < x ? t + 1 : t; }
static double jround(double x) { double r = jfloor(x); return x - r >= 0.5 ? r + 1 : r; }

double snd_now(snd *s) {
  machine *m = s->m;
  if (m->wall) return plat_now_us() - (double)m->wall_base * 1000;
  return (double)m->cycle * 1000 / m->cycles_per_ms + (double)m->sleep_ms * 1000;
}

snd *snd_new(machine *m, u32 rate, int live) {
  snd *s = xmalloc(sizeof *s);
  s->m = m; s->live = live;
  s->clock = 1000000; s->cursor = 0;
  s->pcm_rate = 8000; s->pcm_vol = 0xffff;
  s->master = 0xff;
  s->rate = rate;
  s->opl = opl_new(rate);
  s->gain = 1;
  /* live: the events of now heard this long after now (snd_fill: past
   * the frame the card plays, not the last one made, so that a lead that
   * grows or shrinks moves nothing); --snd-lead sets it, its lead + 20 ms */
  s->latency = live ? (i64)rate * snd_live_latency_ms / 1000 : 0;
  s->slack = (i64)jround(0.05 * rate);
  return s;
}

/* ---- the mixer (snd-mix.js SoundMixer) ---- */
static i64 frame_of(snd *s, double t) { return (i64)jround(t * s->rate / 1e6 + s->offset); }
/* an event, kept in time order: OPL writes are stamped ahead, at the
 * stream's cursor, and the rest at now */
static void push(snd *s, evt e) {
  if (s->qn == s->qcap) {
    if (s->head > 0) {                        /* the applied ones go first */
      memmove(s->q, s->q + s->head, (size_t)(s->qn - s->head) * sizeof(evt));
      s->qn -= s->head; s->head = 0;
    }
    if (s->qn == s->qcap) { s->qcap = s->qcap ? s->qcap * 2 : 4096; s->q = xrealloc(s->q, (size_t)s->qcap * sizeof(evt)); }
  }
  u32 j = s->qn;
  while (j > s->head && s->q[j - 1].t > e.t) j--;
  if (j < s->qn) memmove(s->q + j + 1, s->q + j, (size_t)(s->qn - j) * sizeof(evt));
  s->q[j] = e;
  s->qn++;
}
static void set_vol(voice *v, i32 x) { v->vl = (x & 0xff) / 255.0; v->vr = ((x >> 8) & 0xff) / 255.0; }
static void voice_off(voice *v) { xfree(v->data); v->data = 0; }
static void apply(snd *s, evt *e) {
  switch (e->kind) {
    case EV_OPL: opl_write(s->opl, e->a, e->b); break;
    case EV_PCM: {
      if ((double)frame_of(s, e->t) < (double)s->frame - PCM_STALE * s->rate) { xfree(e->bytes); break; }
      voice *v = &s->v[e->a & (CHANNELS - 1)];
      xfree(v->data);
      v->data = e->bytes; v->len = e->len; v->pos = 0; v->step = (double)e->b / s->rate;
      set_vol(v, e->c);
      break;
    }
    case EV_STOP:
      if (e->a < 0) { for (int i = 0; i < CHANNELS; i++) voice_off(&s->v[i]); }
      else voice_off(&s->v[e->a & (CHANNELS - 1)]);
      break;
    case EV_VOL: set_vol(&s->v[e->a & (CHANNELS - 1)], e->b); break;
    case EV_SPK: s->spk_freq = e->a > 0 ? 1193180.0 / e->a : 0; break;
    case EV_MASTER: s->gain = e->a & MUTE ? 0 : (e->a & 0xff) / 255.0; break;
    case EV_RESET:
      opl_reset(s->opl, s->rate);
      for (int i = 0; i < CHANNELS; i++) voice_off(&s->v[i]);
      s->spk_freq = 0;
      break;
  }
  e->bytes = 0;
}
/* n frames (at most BLOCK) of stereo into L and R from at */
static void segment(snd *s, float *L, float *R, u32 at, u32 n) {
  i32 *ob = s->obuf;
  int stereo = opl_stereo(s->opl);
  opl_generate(s->opl, n, ob);
  if (stereo) for (u32 i = 0; i < n; i++) { L[at + i] = (float)(ob[i * 2] * OPL_GAIN); R[at + i] = (float)(ob[i * 2 + 1] * OPL_GAIN); }
  else for (u32 i = 0; i < n; i++) { float x = (float)(ob[i] * OPL_GAIN); L[at + i] = x; R[at + i] = x; }
  for (int c = 0; c < CHANNELS; c++) {
    voice *v = &s->v[c];
    if (!v->data) continue;
    const u8 *d = v->data;
    u32 len = v->len;
    double pos = v->pos;
    for (u32 i = 0; i < n; i++) {
      u32 k = (u32)(i32)pos;
      if (k >= len) { voice_off(v); break; }
      double f = pos - k;
      i32 a = d[k] - 128, b = (k + 1 < len ? d[k + 1] : 128) - 128;
      double x = (a + (b - a) * f) * PCM_GAIN;
      L[at + i] = (float)((double)L[at + i] + x * v->vl);
      R[at + i] = (float)((double)R[at + i] + x * v->vr);
      pos += v->step;
    }
    v->pos = pos;
  }
  if (s->spk_freq > 0) {
    double inc = s->spk_freq / s->rate, ph = s->spk_phase;
    for (u32 i = 0; i < n; i++) {
      double x = ph < 0.5 ? SPK_AMP : -SPK_AMP;
      L[at + i] = (float)((double)L[at + i] + x); R[at + i] = (float)((double)R[at + i] + x);
      ph += inc; if (ph >= 1) ph -= 1;
    }
    s->spk_phase = ph;
  }
  double g = s->gain;
  for (u32 i = at; i < at + n; i++) {
    double l = L[i], r = R[i];
    L[i] = (float)((l > 1 ? 1 : l < -1 ? -1 : l) * g);
    R[i] = (float)((r > 1 ? 1 : r < -1 ? -1 : r) * g);
  }
}
/* the next n frames: each event applied at its frame, between segments */
static void render(snd *s, float *L, float *R, u32 n) {
  u32 at = 0;
  while (at < n) {
    if (s->head < s->qn) {
      i64 nf = frame_of(s, s->q[s->head].t) - s->frame;
      if (nf <= 0) {
        if (nf < 0 && s->live) { s->late_n++; if (-nf > s->late_most) s->late_most = -nf; }
        apply(s, &s->q[s->head++]);
        continue;
      }
      u32 k = n - at;
      if ((i64)k > nf) k = (u32)nf;
      if (k > BLOCK) k = BLOCK;
      segment(s, L, R, at, k);
      at += k; s->frame += k;
    } else {
      u32 k = n - at < BLOCK ? n - at : BLOCK;
      segment(s, L, R, at, k);
      at += k; s->frame += k;
    }
  }
  if (s->head > 4096) {
    memmove(s->q, s->q + s->head, (size_t)(s->qn - s->head) * sizeof(evt));
    s->qn -= s->head; s->head = 0;
  }
}
static inline int16_t to16(float x) { return (int16_t)(i32)jround((double)x * 32767); }

u32 snd_live_latency_ms = 60;
void snd_fill(snd *s, int16_t *out, u32 n) { snd_fill_ahead(s, out, n, 0); }
/* AHEAD: the frames the card has still to play before out's first */
void snd_fill_ahead(snd *s, int16_t *out, u32 n, u32 ahead) {
  /* where now falls: latency frames past the frame the card plays now.
   * Placed past the last frame made, as it was, a lead grown by a slow
   * poll or a coarse position (ES1371's 4096 frames) put every sound that
   * much later, and its shrinking drifted them back until a resync threw
   * the waiting ones onto the new: late, then piled up (the user's HD
   * Audio and ES1371, 2026-11-24) */
  double now = snd_now(s);
  double want = (double)(s->frame - (i64)ahead + s->latency), got = now * s->rate / 1e6 + s->offset;
  if (!s->synced || got - want > s->slack || want - got > s->slack) {
    s->offset = want - now * s->rate / 1e6;
    s->synced = 1; s->resyncs++;
  } else if (ahead) {
    /* a card whose samples go a little faster or slower than the clock:
     * followed a 64th of the way each fill, so that now stays latency
     * past its playing (its place known to its DMA's step, which this
     * evens out) rather than drifting to the slack and jumping back */
    s->offset += (want - got) / 64;
  }
  while (n > 0) {
    u32 k = n < 4096 ? n : 4096;
    render(s, s->L, s->R, k);
    for (u32 i = 0; i < k; i++) { out[2 * i] = to16(s->L[i]); out[2 * i + 1] = to16(s->R[i]); }
    out += 2 * k; n -= k;
  }
}

/* since the last: events late, the most late (ms), resyncs; started again */
void snd_lateness(snd *s, u32 *n, u32 *most_ms, u32 *resyncs) {
  *n = s->late_n; *most_ms = (u32)(s->late_most * 1000 / s->rate); *resyncs = s->resyncs;
  s->late_n = 0; s->late_most = 0; s->resyncs = 0;
}
void snd_free(snd *s) {
  if (!s) return;
  for (u32 i = s->head; i < s->qn; i++) if (s->q[i].kind == EV_PCM) xfree(s->q[i].bytes);
  xfree(s->q);
  for (int c = 0; c < CHANNELS; c++) xfree(s->v[c].data);
  xfree(s->opl);
  xfree(s);
}

static void put32(u8 *p, u32 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); p[2] = (u8)(v >> 16); p[3] = (u8)(v >> 24); }
u8 *snd_wav(snd *s, u32 *plen) {
  double end = snd_now(s);
  if (s->cursor > end) end = s->cursor;
  i64 fr = (i64)jceil(end * s->rate / 1e6);
  u32 frames = fr > 0 ? (u32)fr : 0;
  u8 *out = xmalloc(44 + (size_t)frames * 4);
  memcpy(out, "RIFF", 4); put32(out + 4, 36 + frames * 4); memcpy(out + 8, "WAVEfmt ", 8);
  put32(out + 16, 16); out[20] = 1; out[22] = 2;
  put32(out + 24, s->rate); put32(out + 28, s->rate * 4); out[32] = 4; out[34] = 16;
  memcpy(out + 36, "data", 4); put32(out + 40, frames * 4);
  for (u32 f = 0; f < frames; f += 4096) {
    u32 n = frames - f < 4096 ? frames - f : 4096;
    render(s, s->L, s->R, n);
    for (u32 i = 0; i < n; i++) {
      int16_t l = to16(s->L[i]), r = to16(s->R[i]);
      u8 *p = out + 44 + (size_t)(f + i) * 4;
      p[0] = (u8)l; p[1] = (u8)((u16)l >> 8); p[2] = (u8)r; p[3] = (u8)((u16)r >> 8);
    }
  }
  *plen = 44 + frames * 4;
  return out;
}

/* ---- the device (snd-device.js SoundDevice) ---- */
static double at_cursor(snd *s) {
  double n = snd_now(s);
  if (s->cursor < n) s->cursor = n;
  return s->cursor;
}
/* a sample on channel ch, now: PCM_PLAY's, or PCM_START's */
static void play(snd *s, i32 ch, u32 addr, i32 len, i32 rate, i32 vol) {
  int c = ch & (CHANNELS - 1);
  /* inside the arena, or nothing: never a copy through a wild pointer */
  if (len <= 0 || len > PCM_MAX) return;
  /* the task's own samples, wherever they are in memory, or its page fault */
  u8 *bytes = xmalloc((size_t)len);
  if (dev_dma_read(s->m, addr, (u32)len, bytes) != (u32)len) { xfree(bytes); return; }
  double t = snd_now(s);
  evt e = {EV_PCM, t, c, rate, vol, bytes, (u32)len};
  push(s, e);
  s->ends[c] = t + len * 1e6 / rate;
  s->plays++;
}
static i32 clamp_rate(i32 v) { return v < 100 ? 100 : v > 200000 ? 200000 : v; }

/* AHEAD read on another CPU (par.c), while CPU 0 may be moving the
 * cursor: the cursor (read until two reads agree: a double is two words
 * on i386), moved on by that CPU's own writes CPU 0 has yet to apply
 * (posted: its queue's entries H to T) as they will move it -- an OPL
 * write brings it to now, a WAIT puts it on, a CLOCK changes WAIT's unit */
i32 snd_ahead(snd *s, const u32 *qa, const i32 *qv, u32 h, u32 t, u32 mask) {
  volatile double *cp = &s->cursor;
  double c0, c1 = *cp;
  do { c0 = c1; c1 = *cp; } while (c0 != c1);
  double now = snd_now(s), c = c0;
  i32 clk = s->clock;
  for (; h != t; h++) {
    u32 a = qa[h & mask]; i32 v = qv[h & mask];
    if (a == OPL) { if (c < now) c = now; }
    else if (a == CLOCK) clk = v > 1 ? v : 1;
    else if (a == WAIT && v > 0) { if (c < now) c = now; c += v * 1e6 / clk; }
  }
  double d = jround(c - now);
  return d <= 0 ? 0 : d >= 2147483647.0 ? 0x7fffffff : (i32)d;
}

i32 snd_read32(snd *s, u32 addr) {
  switch (addr) {
    case CAPS: return 1 | 2 | 4 | 8 | 16 | HAS_START;
    case AHEAD: {
      double d = jround(s->cursor - snd_now(s));
      return d <= 0 ? 0 : d >= 2147483647.0 ? 0x7fffffff : (i32)d;
    }
    case CLOCK: return s->clock;
    case PCM_ADDR: return (i32)s->pcm_addr;
    case PCM_LEN: return s->pcm_len;
    case PCM_RATE: return s->pcm_rate;
    case PCM_VOL: return s->pcm_vol;
    case MASTER: return s->master;
    case PCM_BUSY: {
      double n = snd_now(s);
      i32 b = 0;
      for (int c = 0; c < CHANNELS; c++) if (s->ends[c] > n) b |= 1 << c;
      return b;
    }
    default: return 0;
  }
}

void snd_write32(snd *s, u32 addr, i32 v) {
  switch (addr) {
    case OPL: {
      evt e = {EV_OPL, at_cursor(s), (i32)(((u32)v >> 8) & 0x1ff), v & 0xff, 0, 0, 0};
      push(s, e);
      s->writes++;
      return;
    }
    case MIDI: case MIDI_DATA: return;           /* no synth hears the port */
    case WAIT:
      if (v > 0) s->cursor = at_cursor(s) + v * 1e6 / s->clock;
      return;
    case CLOCK: s->clock = v > 1 ? v : 1; return;
    case PCM_ADDR: s->pcm_addr = (u32)v; return;
    case PCM_LEN: s->pcm_len = v; return;
    case PCM_RATE: s->pcm_rate = clamp_rate(v); return;
    case PCM_VOL: s->pcm_vol = v & 0xffff; return;
    case PCM_PLAY: play(s, v, s->pcm_addr, s->pcm_len, s->pcm_rate, s->pcm_vol); return;
    case PCM_START: {
      /* the block, then its sample: both the writer's, or its page fault.
       * Five of the guest's words: on the 64-bit machine 8 bytes each,
       * their values in the low 32 bits */
      u8 d[5 * WB];
      if (dev_dma_read(s->m, (u32)v, sizeof d, d) != sizeof d) return;
      i32 w[5];
      for (int k = 0; k < 5; k++) w[k] = (i32)(d[k * WB] | d[k * WB + 1] << 8 | d[k * WB + 2] << 16 | (u32)d[k * WB + 3] << 24);
      play(s, w[0], (u32)w[1], w[2], clamp_rate(w[3]), w[4] & 0xffff);
      return;
    }
    case PCM_CHVOL: {
      evt e = {EV_VOL, snd_now(s), v & (CHANNELS - 1), (i32)(((u32)v >> 8) & 0xffff), 0, 0, 0};
      push(s, e);
      return;
    }
    case PCM_STOP: {
      double t = snd_now(s);
      if (v < 0) for (int c = 0; c < CHANNELS; c++) s->ends[c] = 0;
      else s->ends[v & (CHANNELS - 1)] = 0;
      evt e = {EV_STOP, t, v < 0 ? -1 : v & (CHANNELS - 1), 0, 0, 0, 0};
      push(s, e);
      return;
    }
    case PCM_SETVOL: {
      evt e = {EV_VOL, snd_now(s), v & (CHANNELS - 1), s->pcm_vol, 0, 0, 0};
      push(s, e);
      return;
    }
    case SPK: { evt e = {EV_SPK, snd_now(s), v & 0xffff, 0, 0, 0, 0}; push(s, e); return; }
    case RESET: {
      for (int c = 0; c < CHANNELS; c++) s->ends[c] = 0;
      evt e = {EV_RESET, snd_now(s), 0, 0, 0, 0, 0};
      push(s, e);
      return;
    }
    case MASTER: {
      s->master = v & (MUTE | 0xff);
      evt e = {EV_MASTER, snd_now(s), s->master, 0, 0, 0, 0};
      push(s, e);
      return;
    }
    default: return;
  }
}

void snd_stats(snd *s, u32 *writes, u32 *plays) { *writes = s->writes; *plays = s->plays; }

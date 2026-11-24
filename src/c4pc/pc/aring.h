/* aring.h -- what the cards under the machine's sound card share
 * (sb16.c, hda.c, es137x.c): a ring of 16-bit stereo frames at 44100 Hz
 * that the card reads by DMA, mixed a short lead past where it reads.
 *
 * The lead is 40 ms, longer only while the polls come further apart (a
 * busy desktop's frame, a guest's slice): twice the most the DMA moved
 * between two polls in the last second or two, and at most most of the
 * ring. A lead of the ring's half, as it was, put 46 ms and more between
 * a game's sound and its picture; on a card whose position moves a large
 * step at a time it was behind the DMA, which played the ring's last
 * round again: late and choppy (HD Audio in VMware, 2026-11-24).
 *
 * What is mixed is the machine heard: an attached guest's, else VM 0's
 * (hv_snd_fill, hv.c). c4pc's log has a line five seconds in and every
 * 30 s after, and one for each second the card fell behind (at most a
 * hundred), saying how it keeps up:
 *   c4pc: hda: 40 ms ahead (least 31), polls every 4 ms (most 22), the
 *   DMA's steps 7 to 256 frames, 0 underruns (0 in all) */
#ifndef C4PC_ARING_H
#define C4PC_ARING_H
#include "../snd.h"

#define AR_RATE 44100
#define AR_RING 8192                   /* frames round the ring: 186 ms */
#define AR_LEAD ar_lead_frames         /* the least lead: 40 ms, or --snd-lead's (pc.c) */
extern u32 ar_lead_frames;

typedef struct {
  const char *name; snd *card; int16_t *ring;
  u32 played, made, last_pos, underruns;
  u32 step_max, step_prev;             /* the DMA's largest step this second, and last second */
  double at_us, sec_us, rep_us, t0_us;
  /* since the last line */
  u32 polls, r_step_min, r_step_max, r_lead_min, r_und, lines, sec_und;
  double r_gap, r_first;
} aring;

void hv_snd_fill(snd *s0, int16_t *out, u32 n, u32 ahead);    /* hv.c: the attached guest's mix, else s0's */
void pc_log(const char *s);
void pc_logn(i64 v);
double plat_now_us(void);

static void ar_start(aring *a, const char *name, snd *card, int16_t *ring) {
  memset(a, 0, sizeof *a);
  a->name = name; a->card = card; a->ring = ring;
  a->r_step_min = ~0u; a->r_lead_min = ~0u;
}
static u32 ar_lead(aring *a) {
  u32 m = a->step_max > a->step_prev ? a->step_max : a->step_prev;
  u32 l = 2 * m + 256;
  if (l < AR_LEAD) l = AR_LEAD;
  if (l > AR_RING - 1024) l = AR_RING - 1024;
  return l;
}
static void ar_line(aring *a, double now) {
  double span = now - a->r_first;
  pc_log("c4pc: "); pc_log(a->name); pc_log(": ");
  pc_logn((i64)ar_lead(a) * 1000 / AR_RATE); pc_log(" ms ahead (least ");
  pc_logn(a->r_lead_min == ~0u ? 0 : (i64)a->r_lead_min * 1000 / AR_RATE); pc_log("), polls every ");
  pc_logn(a->polls ? (i64)(span / a->polls / 1000) : 0); pc_log(" ms (most ");
  pc_logn((i64)(a->r_gap / 1000)); pc_log("), the DMA's steps ");
  pc_logn(a->r_step_min == ~0u ? 0 : (i64)a->r_step_min); pc_log(" to "); pc_logn((i64)a->r_step_max);
  pc_log(" frames, "); pc_logn((i64)a->r_und); pc_log(" underruns ("); pc_logn((i64)a->underruns); pc_log(" in all)");
  /* VM 0's mix: events that came after their time had been mixed, and its clocks set again */
  { u32 ln, lm, rs; snd_lateness(a->card, &ln, &lm, &rs);
    pc_log("; the mix: "); pc_logn((i64)ln); pc_log(" events late (most "); pc_logn((i64)lm); pc_log(" ms), ");
    pc_logn((i64)rs); pc_log(" resyncs"); }
  pc_log("\n");
  a->polls = 0; a->r_step_max = 0; a->r_step_min = ~0u; a->r_lead_min = ~0u; a->r_und = 0; a->r_gap = 0; a->r_first = now;
  a->lines++;
}
/* a poll: pos the frame the DMA reads now, round the ring; the ring mixed
 * up to the lead past it */
static void ar_poll(aring *a, u32 pos) {
  double now = plat_now_us();
  if (!a->t0_us) a->t0_us = a->sec_us = a->rep_us = a->r_first = now;
  double gap = a->at_us ? now - a->at_us : 0, went = gap * AR_RATE / 1e6;
  u32 step = (pos - a->last_pos + AR_RING) % AR_RING;
  /* nearly the ring in less time than it takes: the position read a
   * little behind the last one (VMware's SB16: "the DMA's steps 2 to
   * 8174"), not a round played -- taken as one, everything mixed ahead
   * was thrown away and mixed again: a click. Kept where it was */
  if (step > AR_RING - 2048 && went < AR_RING - 2048) step = 0;
  else {
    a->last_pos = pos;
    /* a gap longer than the ring: the rounds the clock says went by */
    while ((double)step + AR_RING / 2 < went) step += AR_RING;
  }
  a->played += step;
  if (step > a->step_max) a->step_max = step;
  if (step > a->r_step_max) a->r_step_max = step;
  if (step && step < a->r_step_min) a->r_step_min = step;
  if (a->at_us && now - a->at_us > a->r_gap) a->r_gap = now - a->at_us;
  a->at_us = now; a->polls++;
  i32 ahead = (i32)(a->made - a->played);
  if (ahead < 0) { a->made = a->played; a->underruns++; a->r_und++; ahead = 0; }
  if ((u32)ahead < a->r_lead_min) a->r_lead_min = (u32)ahead;
  /* a second gone: the lead's window moves on, and a line if it fell behind */
  if (now - a->sec_us >= 1e6) {
    a->sec_us = now; a->step_prev = a->step_max; a->step_max = 0;
    int first = a->rep_us == a->t0_us && now - a->t0_us >= 5e6;
    if (first || now - a->rep_us >= 30e6 || (a->underruns != a->sec_und && a->lines < 100)) { a->rep_us = now; ar_line(a, now); }
    a->sec_und = a->underruns;
  }
  u32 lead = ar_lead(a);
  if ((u32)ahead >= lead) return;
  u32 n = lead - (u32)ahead;
  while (n > 0) {
    u32 at = a->made % AR_RING, k = AR_RING - at < n ? AR_RING - at : n;
    hv_snd_fill(a->card, a->ring + 2 * at, k, a->made - a->played);
    a->made += k; n -= k;
  }
}
#endif

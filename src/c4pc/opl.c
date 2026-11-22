/* opl.c -- the Yamaha YM3812 (OPL2) and YMF262 (OPL3): libjs/opl.js in C,
 * which is DOSBox's dbopl.cpp (Copyright (C) 2002-2010 The DOSBox Team,
 * GNU GPL v2 or later), WAVE_TABLEMUL variant, line for line. The tables
 * dbopl makes with sin() and pow() come from libjs/opl-tables.js
 * (mkopl.py), so that both machines make the same samples, bit for bit.
 *
 *   opl_chip *c = opl_new(44100);
 *   opl_write(c, 0x20, 0x01); ...        register (0x100 up: OPL3's second bank), value
 *   opl_generate(c, n, out);             n samples into out: mono, or stereo pairs once OPL3 is on
 */
#include "c4pc.h"
#include "clib.h"
#include "snd.h"

extern const int16_t OPL_WAVE_TABLE[];
extern const uint16_t OPL_MUL_TABLE[];

#define WAVE_SH 22
#define WAVE_MASK ((1u << WAVE_SH) - 1)
#define LFO_SH (WAVE_SH - 10)
#define LFO_MAX (256u << LFO_SH)
#define ENV_MIN 0
#define ENV_MAX 511
#define ENV_LIMIT ((12 * 256) >> 3)
#define RATE_SH 24
#define RATE_MASK ((1u << RATE_SH) - 1)
#define MUL_SH 16
#define OPLRATE (14318180.0 / 288.0)
#define TREMOLO_TABLE 52
#define SHIFT_KSLBASE 16
#define SHIFT_KEYCODE 24
#define MASK_KSR 0x10
#define MASK_SUSTAIN 0x20
#define MASK_VIBRATO 0x40
enum { OFF, RELEASE, SUSTAIN, DECAY, ATTACK };
/* synth modes, in dbopl's order: the comparisons with sm4Start and sm6Start depend on it */
enum { sm2AM, sm2FM, sm3AM, sm3FM, sm4Start, sm3FMFM, sm3AMFM, sm3FMAM, sm3AMAM, sm6Start, sm2Percussion, sm3Percussion };

static const u8 KslCreateTable[16] = {64, 32, 24, 19, 16, 12, 11, 10, 8, 6, 5, 4, 3, 2, 1, 0};
static const u8 FreqCreateTable[16] = {1, 2, 4, 6, 8, 10, 12, 14, 16, 18, 20, 20, 24, 24, 30, 30};
static const u8 AttackSamplesTable[13] = {69, 55, 46, 40, 35, 29, 23, 20, 19, 15, 11, 10, 9};
static const u8 EnvelopeIncreaseTable[13] = {4, 5, 6, 7, 8, 10, 12, 14, 16, 20, 24, 28, 32};
static const u16 WaveBaseTable[8] = {0x000, 0x200, 0x200, 0x800, 0xa00, 0xc00, 0x100, 0x400};
static const u16 WaveMaskTable[8] = {1023, 1023, 511, 511, 1023, 1023, 512, 1023};
static const u16 WaveStartTable[8] = {512, 0, 0, 0, 0, 512, 512, 256};
static const i32 VibratoTable[8] = {1, 0, 1, 30, 1 - 0x80, 0 - 0x80, 1 - 0x80, 30 - 0x80};
static const u8 KslShiftTable[4] = {31, 1, 2, 0};

static u8 KslTable[8 * 16], TremoloTable[TREMOLO_TABLE];
static i8 ChanIndex[32], OpChan[64], OpNum[64];   /* dbopl's ChanOffsetTable and OpOffsetTable */
static int tables_made;
static void init_tables(void) {
  if (tables_made) return;
  tables_made = 1;
  for (int oct = 0; oct < 8; oct++) {
    int base = oct * 8;
    for (int i = 0; i < 16; i++) { int v = base - KslCreateTable[i]; KslTable[oct * 16 + i] = (u8)((v > 0 ? v : 0) * 4); }
  }
  for (int i = 0; i < TREMOLO_TABLE / 2; i++) { TremoloTable[i] = (u8)i; TremoloTable[TREMOLO_TABLE - 1 - i] = (u8)i; }
  for (int i = 0; i < 32; i++) {
    int index = i & 0xf;
    if (index >= 9) { ChanIndex[i] = -1; continue; }
    if (index < 6) index = (index % 3) * 2 + index / 3;
    if (i >= 16) index += 9;
    ChanIndex[i] = (i8)index;
  }
  for (int i = 0; i < 64; i++) {
    if (i % 8 >= 6 || (i / 8) % 4 == 3) { OpChan[i] = -1; continue; }
    int chNum = (i / 8) * 3 + (i % 8) % 3;
    if (chNum >= 12) chNum += 16 - 12;
    OpChan[i] = ChanIndex[chNum];
    OpNum[i] = (i8)((i % 8) / 3);
  }
}

typedef struct opl_op {
  i32 waveBase; u32 waveMask, waveStart, waveIndex, waveAdd, waveCurrent;
  u32 chanData, freqMul, vibrato;
  i32 sustainLevel, totalLevel; u32 currentLevel; i32 volume;
  u32 attackAdd, decayAdd, releaseAdd, rateIndex;
  i32 rateZero, keyOn;
  i32 reg20, reg40, reg60, reg80, regE0;
  i32 state, tremoloMask, vibStrength, ksr;
} opl_op;
typedef struct opl_chan {
  int n; opl_op op[2];
  i32 mode; u32 chanData; i32 old0, old1;
  i32 feedback, regB0, regC0, fourMask, maskLeft, maskRight;
} opl_chan;
struct opl_chip {
  u32 lfoCounter, lfoAdd, noiseCounter, noiseAdd, noiseValue;
  u32 freqMul[16], linearRates[76], attackRates[76];
  opl_chan chan[18];
  i32 reg104, reg08, reg04, regBD;
  i32 vibratoIndex, tremoloIndex, vibratoSign, vibratoShift;
  i32 tremoloValue, vibratoStrength, tremoloStrength;
  i32 waveFormMask, opl3Active;
  u32 rate;
};

static void envelope_select(int val, int *index, int *shift) {
  if (val < 13 * 4) { *index = val & 3; *shift = 12 - (val >> 2); }
  else if (val < 15 * 4) { *index = val - 12 * 4; *shift = 0; }
  else { *index = 12; *shift = 0; }
}
#define SHL32(x, n) ((u32)(x) << ((n) & 31))

/* ---- the operator ---- */
static void op_update_attack(opl_op *o, opl_chip *c) {
  i32 rate = o->reg60 >> 4;
  if (rate) { o->attackAdd = c->attackRates[(rate << 2) + o->ksr]; o->rateZero &= ~(1 << ATTACK); }
  else { o->attackAdd = 0; o->rateZero |= 1 << ATTACK; }
}
static void op_update_decay(opl_op *o, opl_chip *c) {
  i32 rate = o->reg60 & 0xf;
  if (rate) { o->decayAdd = c->linearRates[(rate << 2) + o->ksr]; o->rateZero &= ~(1 << DECAY); }
  else { o->decayAdd = 0; o->rateZero |= 1 << DECAY; }
}
static void op_update_release(opl_op *o, opl_chip *c) {
  i32 rate = o->reg80 & 0xf;
  if (rate) {
    o->releaseAdd = c->linearRates[(rate << 2) + o->ksr];
    o->rateZero &= ~(1 << RELEASE);
    if (!(o->reg20 & MASK_SUSTAIN)) o->rateZero &= ~(1 << SUSTAIN);
  } else {
    o->rateZero |= 1 << RELEASE;
    o->releaseAdd = 0;
    if (!(o->reg20 & MASK_SUSTAIN)) o->rateZero |= 1 << SUSTAIN;
  }
}
static void op_update_attenuation(opl_op *o) {
  i32 kslBase = (o->chanData >> SHIFT_KSLBASE) & 0xff;
  i32 tl = o->reg40 & 0x3f;
  i32 kslShift = KslShiftTable[o->reg40 >> 6];
  o->totalLevel = (tl << 2) + (kslBase >> kslShift);
}
static void op_update_frequency(opl_op *o) {
  u32 freq = o->chanData & ((1 << 10) - 1);
  u32 block = (o->chanData >> 10) & 0xff;
  o->waveAdd = SHL32(freq, block) * o->freqMul;
  if (o->reg20 & MASK_VIBRATO) {
    o->vibStrength = (i32)((freq >> 7) & 0xff);
    o->vibrato = SHL32(o->vibStrength, block) * o->freqMul;
  } else {
    o->vibStrength = 0;
    o->vibrato = 0;
  }
}
static void op_update_rates(opl_op *o, opl_chip *c) {
  i32 newKsr = (o->chanData >> SHIFT_KEYCODE) & 0xff;
  if (!(o->reg20 & MASK_KSR)) newKsr >>= 2;
  if (o->ksr == newKsr) return;
  o->ksr = newKsr;
  op_update_attack(o, c);
  op_update_decay(o, c);
  op_update_release(o, c);
}
static inline u32 op_rate_forward(opl_op *o, u32 add) {
  o->rateIndex += add;
  u32 ret = o->rateIndex >> RATE_SH;
  o->rateIndex &= RATE_MASK;
  return ret;
}
/* dbopl's TemplateVolume, for whichever state the operator is in */
static inline i32 op_volume_step(opl_op *o) {
  i32 vol = o->volume, change;
  switch (o->state) {
    case OFF:
      return ENV_MAX;
    case ATTACK:
      change = (i32)op_rate_forward(o, o->attackAdd);
      if (!change) return vol;
      vol += (i32)((u32)~vol * (u32)change) >> 3;
      if (vol < ENV_MIN) {
        o->volume = ENV_MIN;
        o->rateIndex = 0;
        o->state = DECAY;
        return ENV_MIN;
      }
      break;
    case DECAY:
      vol += (i32)op_rate_forward(o, o->decayAdd);
      if (vol >= o->sustainLevel) {
        if (vol >= ENV_MAX) { o->volume = ENV_MAX; o->state = OFF; return ENV_MAX; }
        o->rateIndex = 0;
        o->state = SUSTAIN;
      }
      break;
    case SUSTAIN:
      if (o->reg20 & MASK_SUSTAIN) return vol;
      /* in sustain, but not sustaining: a release */
      /* fall through */
    case RELEASE:
      vol += (i32)op_rate_forward(o, o->releaseAdd);
      if (vol >= ENV_MAX) { o->volume = ENV_MAX; o->state = OFF; return ENV_MAX; }
      break;
  }
  o->volume = vol;
  return vol;
}
static inline u32 op_forward_volume(opl_op *o) { return o->currentLevel + (u32)op_volume_step(o); }
static inline u32 op_forward_wave(opl_op *o) { o->waveIndex += o->waveCurrent; return o->waveIndex >> WAVE_SH; }
static void op_write20(opl_op *o, opl_chip *c, i32 val) {
  i32 change = o->reg20 ^ val;
  if (!change) return;
  o->reg20 = val;
  o->tremoloMask = (val & 0x80) ? 0xff : 0;
  if (change & MASK_KSR) op_update_rates(o, c);
  if ((o->reg20 & MASK_SUSTAIN) || !o->releaseAdd) o->rateZero |= 1 << SUSTAIN;
  else o->rateZero &= ~(1 << SUSTAIN);
  if (change & (0xf | MASK_VIBRATO)) {
    o->freqMul = c->freqMul[val & 0xf];
    op_update_frequency(o);
  }
}
static void op_write40(opl_op *o, opl_chip *c, i32 val) {
  if (!(o->reg40 ^ val)) return;
  o->reg40 = val;
  op_update_attenuation(o);
}
static void op_write60(opl_op *o, opl_chip *c, i32 val) {
  i32 change = o->reg60 ^ val;
  o->reg60 = val;
  if (change & 0x0f) op_update_decay(o, c);
  if (change & 0xf0) op_update_attack(o, c);
}
static void op_write80(opl_op *o, opl_chip *c, i32 val) {
  i32 change = o->reg80 ^ val;
  if (!change) return;
  o->reg80 = val;
  i32 sustain = val >> 4;
  sustain |= (sustain + 1) & 0x10;
  o->sustainLevel = sustain << 4;
  if (change & 0x0f) op_update_release(o, c);
}
static void op_writeE0(opl_op *o, opl_chip *c, i32 val) {
  if (!(o->regE0 ^ val)) return;
  i32 waveForm = val & ((0x3 & c->waveFormMask) | (0x7 & c->opl3Active));
  o->regE0 = val;
  o->waveBase = WaveBaseTable[waveForm];
  o->waveStart = (u32)WaveStartTable[waveForm] << WAVE_SH;
  o->waveMask = WaveMaskTable[waveForm];
}
static inline int op_silent(opl_op *o) {
  if (!(o->totalLevel + o->volume >= ENV_LIMIT)) return 0;
  if (!(o->rateZero & (1 << o->state))) return 0;
  return 1;
}
static void op_prepare(opl_op *o, opl_chip *c) {
  o->currentLevel = (u32)(o->totalLevel + (c->tremoloValue & o->tremoloMask));
  o->waveCurrent = o->waveAdd;
  if (o->vibStrength >> c->vibratoShift) {
    i32 add = (i32)(o->vibrato >> c->vibratoShift);
    i32 neg = c->vibratoSign;
    add = (add ^ neg) - neg;
    o->waveCurrent += (u32)add;
  }
}
static void op_key_on(opl_op *o, i32 mask) {
  if (!o->keyOn) {
    o->waveIndex = o->waveStart;
    o->rateIndex = 0;
    o->state = ATTACK;
  }
  o->keyOn |= mask;
}
static void op_key_off(opl_op *o, i32 mask) {
  o->keyOn &= ~mask;
  if (!o->keyOn && o->state != OFF) o->state = RELEASE;
}
static inline i32 op_get_wave(opl_op *o, i32 index, u32 vol) {
  return (i32)((u32)(i32)OPL_WAVE_TABLE[o->waveBase + (index & (i32)o->waveMask)] * (u32)OPL_MUL_TABLE[vol]) >> MUL_SH;
}
static inline i32 op_get_sample(opl_op *o, i32 modulation) {
  u32 vol = op_forward_volume(o);
  if (vol >= ENV_LIMIT) {
    o->waveIndex += o->waveCurrent;
    return 0;
  }
  i32 index = (i32)op_forward_wave(o) + modulation;
  return op_get_wave(o, index, vol);
}

/* ---- the channel ---- */
/* dbopl's Op(): operators past the second are the next channels' */
#define OP(ch, c, i) (&(c)->chan[(ch)->n + ((i) >> 1)].op[(i) & 1])
static void ch_set_chan_data(opl_chan *ch, opl_chip *c, u32 data) {
  u32 change = ch->chanData ^ data;
  ch->chanData = data;
  ch->op[0].chanData = data;
  ch->op[1].chanData = data;
  op_update_frequency(&ch->op[0]);
  op_update_frequency(&ch->op[1]);
  if (change & (0xffu << SHIFT_KSLBASE)) { op_update_attenuation(&ch->op[0]); op_update_attenuation(&ch->op[1]); }
  if (change & (0xffu << SHIFT_KEYCODE)) { op_update_rates(&ch->op[0], c); op_update_rates(&ch->op[1], c); }
}
static void ch_update_frequency(opl_chan *ch, opl_chip *c, i32 fourOp) {
  u32 data = ch->chanData & 0xffff;
  u32 kslBase = KslTable[data >> 6];
  u32 keyCode = (data & 0x1c00) >> 9;
  if (c->reg08 & 0x40) keyCode |= (data & 0x100) >> 8;
  else keyCode |= (data & 0x200) >> 9;
  data |= (keyCode << SHIFT_KEYCODE) | (kslBase << SHIFT_KSLBASE);
  ch_set_chan_data(ch, c, data);
  if (fourOp & 0x3f) ch_set_chan_data(ch + 1, c, data);
}
static void ch_writeA0(opl_chan *ch, opl_chip *c, i32 val) {
  i32 fourOp = c->reg104 & c->opl3Active & ch->fourMask;
  if (fourOp > 0x80) return;
  u32 change = (ch->chanData ^ (u32)val) & 0xff;
  if (change) { ch->chanData ^= change; ch_update_frequency(ch, c, fourOp); }
}
static void ch_writeB0(opl_chan *ch, opl_chip *c, i32 val) {
  i32 fourOp = c->reg104 & c->opl3Active & ch->fourMask;
  if (fourOp > 0x80) return;
  u32 change = (ch->chanData ^ ((u32)val << 8)) & 0x1f00;
  if (change) { ch->chanData ^= change; ch_update_frequency(ch, c, fourOp); }
  if (!((val ^ ch->regB0) & 0x20)) return;
  ch->regB0 = val;
  if (val & 0x20) {
    op_key_on(&ch->op[0], 1); op_key_on(&ch->op[1], 1);
    if (fourOp & 0x3f) { op_key_on(&ch[1].op[0], 1); op_key_on(&ch[1].op[1], 1); }
  } else {
    op_key_off(&ch->op[0], 1); op_key_off(&ch->op[1], 1);
    if (fourOp & 0x3f) { op_key_off(&ch[1].op[0], 1); op_key_off(&ch[1].op[1], 1); }
  }
}
static void ch_writeC0(opl_chan *ch, opl_chip *c, i32 val) {
  i32 change = val ^ ch->regC0;
  if (!change) return;
  ch->regC0 = val;
  ch->feedback = (val >> 1) & 7;
  ch->feedback = ch->feedback ? 9 - ch->feedback : 31;
  if (c->opl3Active) {
    if ((c->reg104 & ch->fourMask) & 0x3f) {
      opl_chan *chan0, *chan1;
      if (!(ch->fourMask & 0x80)) { chan0 = ch; chan1 = ch + 1; }
      else { chan0 = ch - 1; chan1 = ch; }
      i32 synth = (chan0->regC0 & 1) | ((chan1->regC0 & 1) << 1);
      static const i32 modes[4] = {sm3FMFM, sm3AMFM, sm3FMAM, sm3AMAM};
      chan0->mode = modes[synth];
    } else if ((ch->fourMask & 0x40) && (c->regBD & 0x20)) {
      /* percussion: left alone */
    } else if (val & 1) {
      ch->mode = sm3AM;
    } else {
      ch->mode = sm3FM;
    }
    ch->maskLeft = (val & 0x10) ? -1 : 0;
    ch->maskRight = (val & 0x20) ? -1 : 0;
  } else {
    if ((ch->fourMask & 0x40) && (c->regBD & 0x20)) {
      /* percussion: left alone */
    } else if (val & 1) {
      ch->mode = sm2AM;
    } else {
      ch->mode = sm2FM;
    }
  }
}
static void ch_resetC0(opl_chan *ch, opl_chip *c) {
  i32 val = ch->regC0;
  ch->regC0 ^= 0xff;
  ch_writeC0(ch, c, val);
}
static u32 chip_forward_noise(opl_chip *c) {
  c->noiseCounter += c->noiseAdd;
  u32 count = c->noiseCounter >> LFO_SH;
  c->noiseCounter &= WAVE_MASK;
  for (; count > 0; --count) {
    c->noiseValue ^= 0x800302u & (0u - (c->noiseValue & 1));
    c->noiseValue >>= 1;
  }
  return c->noiseValue;
}
static void ch_generate_percussion(opl_chan *ch, opl_chip *c, i32 *output, int at, int opl3) {
  i32 mod0 = (i32)((u32)(ch->old0 + ch->old1) >> ch->feedback);
  ch->old0 = ch->old1;
  ch->old1 = op_get_sample(OP(ch, c, 0), mod0);
  i32 mod = (ch->regC0 & 1) ? 0 : ch->old0;
  i32 sample = op_get_sample(OP(ch, c, 1), mod);
  u32 noiseBit = chip_forward_noise(c) & 1;
  u32 c2 = op_forward_wave(OP(ch, c, 2));
  u32 c5 = op_forward_wave(OP(ch, c, 5));
  u32 phaseBit = (((c2 & 0x88) ^ ((c2 << 5) & 0x80)) | ((c5 ^ (c5 << 2)) & 0x20)) ? 0x02 : 0x00;
  u32 hhVol = op_forward_volume(OP(ch, c, 2));
  if (!(hhVol >= ENV_LIMIT)) {
    u32 hhIndex = (phaseBit << 8) | (0x34u << (phaseBit ^ (noiseBit << 1)));
    sample += op_get_wave(OP(ch, c, 2), (i32)hhIndex, hhVol);
  }
  u32 sdVol = op_forward_volume(OP(ch, c, 3));
  if (!(sdVol >= ENV_LIMIT)) {
    u32 sdIndex = (0x100 + (c2 & 0x100)) ^ (noiseBit << 8);
    sample += op_get_wave(OP(ch, c, 3), (i32)sdIndex, sdVol);
  }
  sample += op_get_sample(OP(ch, c, 4), 0);
  u32 tcVol = op_forward_volume(OP(ch, c, 5));
  if (!(tcVol >= ENV_LIMIT)) {
    u32 tcIndex = (1 + phaseBit) << 8;
    sample += op_get_wave(OP(ch, c, 5), (i32)tcIndex, tcVol);
  }
  sample = (i32)((u32)sample << 1);
  if (opl3) { output[at] += sample; output[at + 1] += sample; }
  else output[at] += sample;
}
/* dbopl's BlockTemplate<mode>: samples into output from at; the next channel's number */
static int ch_block(opl_chan *ch, opl_chip *c, u32 samples, i32 *output, int at) {
  i32 mode = ch->mode;
  switch (mode) {
    case sm2AM: case sm3AM:
      if (op_silent(OP(ch, c, 0)) && op_silent(OP(ch, c, 1))) { ch->old0 = ch->old1 = 0; return ch->n + 1; }
      break;
    case sm2FM: case sm3FM:
      if (op_silent(OP(ch, c, 1))) { ch->old0 = ch->old1 = 0; return ch->n + 1; }
      break;
    case sm3FMFM:
      if (op_silent(OP(ch, c, 3))) { ch->old0 = ch->old1 = 0; return ch->n + 2; }
      break;
    case sm3AMFM:
      if (op_silent(OP(ch, c, 0)) && op_silent(OP(ch, c, 3))) { ch->old0 = ch->old1 = 0; return ch->n + 2; }
      break;
    case sm3FMAM:
      if (op_silent(OP(ch, c, 1)) && op_silent(OP(ch, c, 3))) { ch->old0 = ch->old1 = 0; return ch->n + 2; }
      break;
    case sm3AMAM:
      if (op_silent(OP(ch, c, 0)) && op_silent(OP(ch, c, 2)) && op_silent(OP(ch, c, 3))) { ch->old0 = ch->old1 = 0; return ch->n + 2; }
      break;
  }
  op_prepare(OP(ch, c, 0), c);
  op_prepare(OP(ch, c, 1), c);
  if (mode > sm4Start) { op_prepare(OP(ch, c, 2), c); op_prepare(OP(ch, c, 3), c); }
  if (mode > sm6Start) { op_prepare(OP(ch, c, 4), c); op_prepare(OP(ch, c, 5), c); }
  for (u32 i = 0; i < samples; i++) {
    if (mode == sm2Percussion) { ch_generate_percussion(ch, c, output, at + (int)i, 0); continue; }
    if (mode == sm3Percussion) { ch_generate_percussion(ch, c, output, at + (int)i * 2, 1); continue; }
    i32 mod = (i32)((u32)(ch->old0 + ch->old1) >> ch->feedback);
    ch->old0 = ch->old1;
    ch->old1 = op_get_sample(OP(ch, c, 0), mod);
    i32 sample = 0, next;
    i32 out0 = ch->old0;
    if (mode == sm2AM || mode == sm3AM) {
      sample = out0 + op_get_sample(OP(ch, c, 1), 0);
    } else if (mode == sm2FM || mode == sm3FM) {
      sample = op_get_sample(OP(ch, c, 1), out0);
    } else if (mode == sm3FMFM) {
      next = op_get_sample(OP(ch, c, 1), out0);
      next = op_get_sample(OP(ch, c, 2), next);
      sample = op_get_sample(OP(ch, c, 3), next);
    } else if (mode == sm3AMFM) {
      sample = out0;
      next = op_get_sample(OP(ch, c, 1), 0);
      next = op_get_sample(OP(ch, c, 2), next);
      sample += op_get_sample(OP(ch, c, 3), next);
    } else if (mode == sm3FMAM) {
      sample = op_get_sample(OP(ch, c, 1), out0);
      next = op_get_sample(OP(ch, c, 2), 0);
      sample += op_get_sample(OP(ch, c, 3), next);
    } else if (mode == sm3AMAM) {
      sample = out0;
      next = op_get_sample(OP(ch, c, 1), 0);
      sample += op_get_sample(OP(ch, c, 2), next);
      sample += op_get_sample(OP(ch, c, 3), 0);
    }
    if (mode == sm2AM || mode == sm2FM) {
      output[at + i] += sample;
    } else {
      output[at + i * 2] += sample & ch->maskLeft;
      output[at + i * 2 + 1] += sample & ch->maskRight;
    }
  }
  if (mode <= sm3FM) return ch->n + 1;
  if (mode <= sm3AMAM) return ch->n + 2;
  return ch->n + 3;
}

/* ---- the chip ---- */
static u32 chip_forward_lfo(opl_chip *c, u32 samples) {
  i32 v = VibratoTable[c->vibratoIndex >> 2];
  c->vibratoSign = v >> 7;
  c->vibratoShift = ((v & 7) + c->vibratoStrength) & 0xff;
  c->tremoloValue = TremoloTable[c->tremoloIndex] >> c->tremoloStrength;
  u32 todo = LFO_MAX - c->lfoCounter;
  u64 count = ((u64)todo + c->lfoAdd - 1) / c->lfoAdd;
  if (count > samples) {
    count = samples;
    c->lfoCounter = (u32)(c->lfoCounter + count * c->lfoAdd);
  } else {
    c->lfoCounter = (u32)(c->lfoCounter + count * c->lfoAdd);
    c->lfoCounter &= LFO_MAX - 1;
    c->vibratoIndex = (c->vibratoIndex + 1) & 31;
    if (c->tremoloIndex + 1 < TREMOLO_TABLE) ++c->tremoloIndex;
    else c->tremoloIndex = 0;
  }
  return (u32)count;
}
static void chip_writeBD(opl_chip *c, i32 val) {
  i32 change = c->regBD ^ val;
  if (!change) return;
  c->regBD = val;
  c->vibratoStrength = (val & 0x40) ? 0x00 : 0x01;
  c->tremoloStrength = (val & 0x80) ? 0x00 : 0x02;
  opl_chan *ch = c->chan;
  if (val & 0x20) {
    if (change & 0x20) ch[6].mode = c->opl3Active ? sm3Percussion : sm2Percussion;
    if (val & 0x10) { op_key_on(&ch[6].op[0], 2); op_key_on(&ch[6].op[1], 2); }
    else { op_key_off(&ch[6].op[0], 2); op_key_off(&ch[6].op[1], 2); }
    if (val & 0x1) op_key_on(&ch[7].op[0], 2); else op_key_off(&ch[7].op[0], 2);
    if (val & 0x8) op_key_on(&ch[7].op[1], 2); else op_key_off(&ch[7].op[1], 2);
    if (val & 0x4) op_key_on(&ch[8].op[0], 2); else op_key_off(&ch[8].op[0], 2);
    if (val & 0x2) op_key_on(&ch[8].op[1], 2); else op_key_off(&ch[8].op[1], 2);
  } else if (change & 0x20) {
    ch_resetC0(&ch[6], c);
    op_key_off(&ch[6].op[0], 2); op_key_off(&ch[6].op[1], 2);
    op_key_off(&ch[7].op[0], 2); op_key_off(&ch[7].op[1], 2);
    op_key_off(&ch[8].op[0], 2); op_key_off(&ch[8].op[1], 2);
  }
}
void opl_write(opl_chip *c, int reg, int val) {
  val &= 0xff;
  int index;
#define REGOP(fn) do { index = ((reg >> 3) & 0x20) | (reg & 0x1f); \
    if (OpChan[index] >= 0) fn(&c->chan[(int)OpChan[index]].op[(int)OpNum[index]], c, val); } while (0)
#define REGCHAN(fn) do { index = ((reg >> 4) & 0x10) | (reg & 0xf); \
    if (ChanIndex[index] >= 0) fn(&c->chan[(int)ChanIndex[index]], c, val); } while (0)
  switch ((reg & 0xf0) >> 4) {
    case 0x0:
      if (reg == 0x01) {
        c->waveFormMask = (val & 0x20) ? 0x7 : 0x0;
      } else if (reg == 0x104) {
        if (!((c->reg104 ^ val) & 0x3f)) return;
        c->reg104 = 0x80 | (val & 0x3f);
      } else if (reg == 0x105) {
        if (!((c->opl3Active ^ val) & 1)) return;
        c->opl3Active = (val & 1) ? 0xff : 0;
        for (int i = 0; i < 18; i++) ch_resetC0(&c->chan[i], c);
      } else if (reg == 0x08) {
        c->reg08 = val;
      }
      /* fall through */
    case 0x1:
      break;
    case 0x2: case 0x3: REGOP(op_write20); break;
    case 0x4: case 0x5: REGOP(op_write40); break;
    case 0x6: case 0x7: REGOP(op_write60); break;
    case 0x8: case 0x9: REGOP(op_write80); break;
    case 0xa: REGCHAN(ch_writeA0); break;
    case 0xb:
      if (reg == 0xbd) chip_writeBD(c, val);
      else REGCHAN(ch_writeB0);
      break;
    case 0xc: REGCHAN(ch_writeC0);
      /* fall through */
    case 0xd:
      break;
    case 0xe: case 0xf: REGOP(op_writeE0); break;
  }
#undef REGOP
#undef REGCHAN
}
int opl_stereo(opl_chip *c) { return c->opl3Active != 0; }

/* total samples into output: OPL2's mono, or OPL3's stereo pairs once
 * register 0x105 has turned it on */
void opl_generate(opl_chip *c, u32 total, i32 *output) {
  int stereo = c->opl3Active != 0, nch = stereo ? 18 : 9, w = stereo ? 2 : 1;
  int at = 0;
  while (total > 0) {
    u32 samples = chip_forward_lfo(c, total);
    memset(output + at, 0, (size_t)samples * w * 4);
    for (int ch = 0; ch < nch;) ch = ch_block(&c->chan[ch], c, samples, output, at);
    total -= samples;
    at += (int)samples * w;
  }
}

static void chip_setup(opl_chip *c, u32 rate) {
  double scale = OPLRATE / (double)rate;
  c->noiseAdd = (u32)(i64)(0.5 + scale * (1 << LFO_SH));
  c->noiseCounter = 0;
  c->noiseValue = 1;
  c->lfoAdd = (u32)(i64)(0.5 + scale * (1 << LFO_SH));
  c->lfoCounter = 0;
  c->vibratoIndex = 0;
  c->tremoloIndex = 0;
  u32 freqScale = (u32)(i64)(0.5 + scale * (1 << (WAVE_SH - 1 - 10)));
  for (int i = 0; i < 16; i++) c->freqMul[i] = freqScale * FreqCreateTable[i];
  for (int i = 0; i < 76; i++) {
    int index, shift; envelope_select(i, &index, &shift);
    c->linearRates[i] = (u32)(i64)(scale * (EnvelopeIncreaseTable[index] << (RATE_SH - shift - 3)));
  }
  for (int i = 0; i < 62; i++) {
    int index, shift; envelope_select(i, &index, &shift);
    i32 original = (i32)(u32)(i64)((double)(AttackSamplesTable[index] << shift) / scale);
    i32 guessAdd = (i32)(u32)(i64)(scale * (EnvelopeIncreaseTable[index] << (RATE_SH - shift - 3)));
    i32 bestAdd = guessAdd;
    i32 bestDiff = 1 << 30;
    for (int passes = 0; passes < 16; passes++) {
      i32 volume = ENV_MAX, samples = 0; u32 count = 0;
      while (volume > 0 && samples < original * 2) {
        count += (u32)guessAdd;
        i32 change = (i32)(count >> RATE_SH);
        count &= RATE_MASK;
        if (change) volume += (i32)((u32)~volume * (u32)change) >> 3;
        samples++;
      }
      i32 diff = original - samples;
      i32 lDiff = diff < 0 ? -diff : diff;
      if (lDiff < bestDiff) {
        bestDiff = lDiff;
        bestAdd = guessAdd;
        if (!bestDiff) break;
      }
      if (diff < 0) {
        i32 mul = original ? (i32)((i64)(i32)((u32)(original - diff) << 12) / original) : 0;
        guessAdd = (i32)((u32)guessAdd * (u32)mul) >> 12;
        guessAdd++;
      } else if (diff > 0) {
        i32 mul = original ? (i32)((i64)(i32)((u32)(original - diff) << 12) / original) : 0;
        guessAdd = (i32)((u32)guessAdd * (u32)mul) >> 12;
        guessAdd--;
      }
    }
    c->attackRates[i] = (u32)bestAdd;
  }
  for (int i = 62; i < 76; i++) c->attackRates[i] = 8u << RATE_SH;
  opl_chan *ch = c->chan;
  ch[0].fourMask = 0x00 | 1; ch[1].fourMask = 0x80 | 1;
  ch[2].fourMask = 0x00 | 2; ch[3].fourMask = 0x80 | 2;
  ch[4].fourMask = 0x00 | 4; ch[5].fourMask = 0x80 | 4;
  ch[9].fourMask = 0x00 | 8; ch[10].fourMask = 0x80 | 8;
  ch[11].fourMask = 0x00 | 16; ch[12].fourMask = 0x80 | 16;
  ch[13].fourMask = 0x00 | 32; ch[14].fourMask = 0x80 | 32;
  ch[6].fourMask = 0x40; ch[7].fourMask = 0x40; ch[8].fourMask = 0x40;
  /* clear everything in OPL3 mode, then in OPL2 mode */
  opl_write(c, 0x105, 0x1);
  for (int i = 0; i < 512; i++) {
    if (i == 0x105) continue;
    opl_write(c, i, 0xff);
    opl_write(c, i, 0x0);
  }
  opl_write(c, 0x105, 0x0);
  for (int i = 0; i < 255; i++) {
    opl_write(c, i, 0xff);
    opl_write(c, i, 0x0);
  }
}

static void op_init(opl_op *o) {
  memset(o, 0, sizeof *o);
  o->sustainLevel = ENV_MAX; o->totalLevel = ENV_MAX; o->currentLevel = ENV_MAX; o->volume = ENV_MAX;
  o->rateZero = 1 << OFF;
  o->state = OFF;
}
void opl_reset(opl_chip *c, u32 rate) {
  init_tables();
  memset(c, 0, sizeof *c);
  for (int i = 0; i < 18; i++) {
    opl_chan *ch = &c->chan[i];
    ch->n = i;
    op_init(&ch->op[0]); op_init(&ch->op[1]);
    ch->mode = sm2FM;
    ch->feedback = 31;
    ch->maskLeft = -1; ch->maskRight = -1;
  }
  c->rate = rate;
  chip_setup(c, rate);
}
opl_chip *opl_new(u32 rate) {
  opl_chip *c = xmalloc(sizeof *c);
  opl_reset(c, rate);
  return c;
}
/* what the OPL holds (cli.js -s, snd-mix.js oplState) */
void opl_state(opl_chip *c, int *opl3, int *reg104, int *regBD) {
  *opl3 = c->opl3Active != 0; *reg104 = c->reg104 & 0x3f; *regBD = c->regBD;
}

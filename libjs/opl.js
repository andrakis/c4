// opl.js - the Yamaha YM3812 (OPL2) and YMF262 (OPL3) FM synthesizers:
// the AdLib, for the sound device (snd-device.js).
//
// A port of DOSBox's dbopl.cpp (Copyright (C) 2002-2010 The DOSBox Team,
// GNU GPL v2 or later), WAVE_TABLEMUL variant, kept line for line so the
// two can be compared: libjs/tests/test-opl.mjs renders the same register
// stream through both and requires the same samples, bit for bit. C's
// unsigned and 8-bit arithmetic is spelled out where it matters (>>> 0,
// & 0xff, Math.imul). The tables that dbopl makes with sin() and pow() are
// opl-tables.js, made by its own C++ so no browser's Math changes a bit.
//
//   const chip = new OplChip(48000);
//   chip.write(0x20, 0x01); ...          // register (0x100 and up: OPL3's second bank), value
//   chip.generate(n, out);               // n samples into an Int32Array: mono, or stereo pairs once OPL3 is on

import { WAVE_TABLE, MUL_TABLE } from './opl-tables.js';

const WAVE_SH = 22, WAVE_MASK = (1 << WAVE_SH) - 1;
const LFO_SH = WAVE_SH - 10, LFO_MAX = 256 << LFO_SH;
const ENV_MIN = 0, ENV_MAX = 511, ENV_LIMIT = (12 * 256) >> 3;
const RATE_SH = 24, RATE_MASK = (1 << RATE_SH) - 1, MUL_SH = 16;
const OPLRATE = 14318180.0 / 288.0;
const TREMOLO_TABLE = 52;
const SHIFT_KSLBASE = 16, SHIFT_KEYCODE = 24;
const MASK_KSR = 0x10, MASK_SUSTAIN = 0x20, MASK_VIBRATO = 0x40;
const OFF = 0, RELEASE = 1, SUSTAIN = 2, DECAY = 3, ATTACK = 4;
// synth modes, in dbopl's order: the comparisons with sm4Start and sm6Start depend on it
const sm2AM = 0, sm2FM = 1, sm3AM = 2, sm3FM = 3, sm4Start = 4, sm3FMFM = 5, sm3AMFM = 6,
      sm3FMAM = 7, sm3AMAM = 8, sm6Start = 9, sm2Percussion = 10, sm3Percussion = 11;

const KslCreateTable = [64, 32, 24, 19, 16, 12, 11, 10, 8, 6, 5, 4, 3, 2, 1, 0];
const FreqCreateTable = [1, 2, 4, 6, 8, 10, 12, 14, 16, 18, 20, 20, 24, 24, 30, 30];
const AttackSamplesTable = [69, 55, 46, 40, 35, 29, 23, 20, 19, 15, 11, 10, 9];
const EnvelopeIncreaseTable = [4, 5, 6, 7, 8, 10, 12, 14, 16, 20, 24, 28, 32];
const WaveBaseTable = [0x000, 0x200, 0x200, 0x800, 0xa00, 0xc00, 0x100, 0x400];
const WaveMaskTable = [1023, 1023, 511, 511, 1023, 1023, 512, 1023];
const WaveStartTable = [512, 0, 0, 0, 0, 512, 512, 256];
const VibratoTable = [1, 0, 1, 30, 1 - 0x80, 0 - 0x80, 1 - 0x80, 30 - 0x80];
const KslShiftTable = [31, 1, 2, 0];

const KslTable = new Uint8Array(8 * 16);
const TremoloTable = new Uint8Array(TREMOLO_TABLE);
const ChanIndex = new Int8Array(32);          // dbopl's ChanOffsetTable, as channel numbers (-1: none)
const OpChan = new Int8Array(64), OpNum = new Int8Array(64);   // its OpOffsetTable
(function initTables() {
  for (let oct = 0; oct < 8; oct++) {
    const base = oct * 8;
    for (let i = 0; i < 16; i++) KslTable[oct * 16 + i] = Math.max(0, base - KslCreateTable[i]) * 4;
  }
  for (let i = 0; i < TREMOLO_TABLE / 2; i++) { TremoloTable[i] = i; TremoloTable[TREMOLO_TABLE - 1 - i] = i; }
  for (let i = 0; i < 32; i++) {
    let index = i & 0xf;
    if (index >= 9) { ChanIndex[i] = -1; continue; }
    if (index < 6) index = (index % 3) * 2 + ((index / 3) | 0);
    if (i >= 16) index += 9;
    ChanIndex[i] = index;
  }
  for (let i = 0; i < 64; i++) {
    if (i % 8 >= 6 || ((i / 8) | 0) % 4 === 3) { OpChan[i] = -1; continue; }
    let chNum = ((i / 8) | 0) * 3 + (i % 8) % 3;
    if (chNum >= 12) chNum += 16 - 12;
    OpChan[i] = ChanIndex[chNum];
    OpNum[i] = ((i % 8) / 3) | 0;
  }
})();

function envelopeSelect(val) {         // [index, shift]
  if (val < 13 * 4) return [val & 3, 12 - (val >> 2)];
  if (val < 15 * 4) return [val - 12 * 4, 0];
  return [12, 0];
}

class Operator {
  constructor() {
    this.waveBase = 0; this.waveMask = 0; this.waveStart = 0;
    this.waveIndex = 0; this.waveAdd = 0; this.waveCurrent = 0;
    this.chanData = 0; this.freqMul = 0; this.vibrato = 0;
    this.sustainLevel = ENV_MAX; this.totalLevel = ENV_MAX; this.currentLevel = ENV_MAX; this.volume = ENV_MAX;
    this.attackAdd = 0; this.decayAdd = 0; this.releaseAdd = 0; this.rateIndex = 0;
    this.rateZero = 1 << OFF; this.keyOn = 0;
    this.reg20 = 0; this.reg40 = 0; this.reg60 = 0; this.reg80 = 0; this.regE0 = 0;
    this.state = OFF; this.tremoloMask = 0; this.vibStrength = 0; this.ksr = 0;
  }
  updateAttack(chip) {
    const rate = this.reg60 >> 4;
    if (rate) { this.attackAdd = chip.attackRates[(rate << 2) + this.ksr]; this.rateZero &= ~(1 << ATTACK); }
    else { this.attackAdd = 0; this.rateZero |= 1 << ATTACK; }
  }
  updateDecay(chip) {
    const rate = this.reg60 & 0xf;
    if (rate) { this.decayAdd = chip.linearRates[(rate << 2) + this.ksr]; this.rateZero &= ~(1 << DECAY); }
    else { this.decayAdd = 0; this.rateZero |= 1 << DECAY; }
  }
  updateRelease(chip) {
    const rate = this.reg80 & 0xf;
    if (rate) {
      this.releaseAdd = chip.linearRates[(rate << 2) + this.ksr];
      this.rateZero &= ~(1 << RELEASE);
      if (!(this.reg20 & MASK_SUSTAIN)) this.rateZero &= ~(1 << SUSTAIN);
    } else {
      this.rateZero |= 1 << RELEASE;
      this.releaseAdd = 0;
      if (!(this.reg20 & MASK_SUSTAIN)) this.rateZero |= 1 << SUSTAIN;
    }
  }
  updateAttenuation() {
    const kslBase = (this.chanData >>> SHIFT_KSLBASE) & 0xff;
    const tl = this.reg40 & 0x3f;
    const kslShift = KslShiftTable[this.reg40 >> 6];
    this.totalLevel = (tl << 2) + (kslBase >> kslShift);
  }
  updateFrequency() {
    const freq = this.chanData & ((1 << 10) - 1);
    const block = (this.chanData >>> 10) & 0xff;
    this.waveAdd = Math.imul(freq << block, this.freqMul) >>> 0;
    if (this.reg20 & MASK_VIBRATO) {
      this.vibStrength = (freq >> 7) & 0xff;
      this.vibrato = Math.imul(this.vibStrength << block, this.freqMul) >>> 0;
    } else {
      this.vibStrength = 0;
      this.vibrato = 0;
    }
  }
  updateRates(chip) {
    let newKsr = (this.chanData >>> SHIFT_KEYCODE) & 0xff;
    if (!(this.reg20 & MASK_KSR)) newKsr >>= 2;
    if (this.ksr === newKsr) return;
    this.ksr = newKsr;
    this.updateAttack(chip);
    this.updateDecay(chip);
    this.updateRelease(chip);
  }
  rateForward(add) {
    this.rateIndex = (this.rateIndex + add) >>> 0;
    const ret = this.rateIndex >>> RATE_SH;
    this.rateIndex &= RATE_MASK;
    return ret;
  }
  // dbopl's TemplateVolume, for whichever state the operator is in
  volumeStep() {
    let vol = this.volume, change;
    switch (this.state) {
      case OFF:
        return ENV_MAX;
      case ATTACK:
        change = this.rateForward(this.attackAdd);
        if (!change) return vol;
        vol += (Math.imul(~vol, change)) >> 3;
        if (vol < ENV_MIN) {
          this.volume = ENV_MIN;
          this.rateIndex = 0;
          this.state = DECAY;
          return ENV_MIN;
        }
        break;
      case DECAY:
        vol += this.rateForward(this.decayAdd);
        if (vol >= this.sustainLevel) {
          if (vol >= ENV_MAX) { this.volume = ENV_MAX; this.state = OFF; return ENV_MAX; }
          this.rateIndex = 0;
          this.state = SUSTAIN;
        }
        break;
      case SUSTAIN:
        if (this.reg20 & MASK_SUSTAIN) return vol;
        // in sustain, but not sustaining: a release
        // falls through
      case RELEASE:
        vol += this.rateForward(this.releaseAdd);
        if (vol >= ENV_MAX) { this.volume = ENV_MAX; this.state = OFF; return ENV_MAX; }
        break;
    }
    this.volume = vol;
    return vol;
  }
  forwardVolume() { return (this.currentLevel + this.volumeStep()) >>> 0; }
  forwardWave() {
    this.waveIndex = (this.waveIndex + this.waveCurrent) >>> 0;
    return this.waveIndex >>> WAVE_SH;
  }
  write20(chip, val) {
    const change = this.reg20 ^ val;
    if (!change) return;
    this.reg20 = val;
    this.tremoloMask = (val & 0x80) ? 0xff : 0;
    if (change & MASK_KSR) this.updateRates(chip);
    if ((this.reg20 & MASK_SUSTAIN) || !this.releaseAdd) this.rateZero |= 1 << SUSTAIN;
    else this.rateZero &= ~(1 << SUSTAIN);
    if (change & (0xf | MASK_VIBRATO)) {
      this.freqMul = chip.freqMul[val & 0xf];
      this.updateFrequency();
    }
  }
  write40(chip, val) {
    if (!(this.reg40 ^ val)) return;
    this.reg40 = val;
    this.updateAttenuation();
  }
  write60(chip, val) {
    const change = this.reg60 ^ val;
    this.reg60 = val;
    if (change & 0x0f) this.updateDecay(chip);
    if (change & 0xf0) this.updateAttack(chip);
  }
  write80(chip, val) {
    const change = this.reg80 ^ val;
    if (!change) return;
    this.reg80 = val;
    let sustain = val >> 4;
    sustain |= (sustain + 1) & 0x10;
    this.sustainLevel = sustain << 4;
    if (change & 0x0f) this.updateRelease(chip);
  }
  writeE0(chip, val) {
    if (!(this.regE0 ^ val)) return;
    const waveForm = val & ((0x3 & chip.waveFormMask) | (0x7 & chip.opl3Active));
    this.regE0 = val;
    this.waveBase = WaveBaseTable[waveForm];
    this.waveStart = (WaveStartTable[waveForm] << WAVE_SH) >>> 0;
    this.waveMask = WaveMaskTable[waveForm];
  }
  silent() {
    if (!(this.totalLevel + this.volume >= ENV_LIMIT)) return false;
    if (!(this.rateZero & (1 << this.state))) return false;
    return true;
  }
  prepare(chip) {
    this.currentLevel = (this.totalLevel + (chip.tremoloValue & this.tremoloMask)) >>> 0;
    this.waveCurrent = this.waveAdd;
    if (this.vibStrength >> chip.vibratoShift) {
      let add = (this.vibrato >>> chip.vibratoShift) | 0;
      const neg = chip.vibratoSign;
      add = (add ^ neg) - neg;
      this.waveCurrent = (this.waveCurrent + add) >>> 0;
    }
  }
  keyOnMask(mask) {
    if (!this.keyOn) {
      this.waveIndex = this.waveStart;
      this.rateIndex = 0;
      this.state = ATTACK;
    }
    this.keyOn |= mask;
  }
  keyOffMask(mask) {
    this.keyOn &= ~mask;
    if (!this.keyOn && this.state !== OFF) this.state = RELEASE;
  }
  getWave(index, vol) {
    return Math.imul(WAVE_TABLE[this.waveBase + (index & this.waveMask)], MUL_TABLE[vol]) >> MUL_SH;
  }
  getSample(modulation) {
    const vol = this.forwardVolume();
    if (vol >= ENV_LIMIT) {
      this.waveIndex = (this.waveIndex + this.waveCurrent) >>> 0;
      return 0;
    }
    const index = this.forwardWave() + modulation;
    return this.getWave(index, vol);
  }
}

class Channel {
  constructor(chip, n) {
    this.chip = chip; this.n = n;
    this.op = [new Operator(), new Operator()];
    this.mode = sm2FM;
    this.chanData = 0;
    this.old0 = 0; this.old1 = 0;
    this.feedback = 31; this.regB0 = 0; this.regC0 = 0; this.fourMask = 0;
    this.maskLeft = -1; this.maskRight = -1;
  }
  // dbopl's Op(): operators past the second are the next channels'
  Op(index) { return this.chip.chan[this.n + (index >> 1)].op[index & 1]; }
  next(k) { return this.chip.chan[this.n + k]; }
  setChanData(chip, data) {
    const change = this.chanData ^ data;
    this.chanData = data;
    this.op[0].chanData = data;
    this.op[1].chanData = data;
    this.op[0].updateFrequency();
    this.op[1].updateFrequency();
    if (change & (0xff << SHIFT_KSLBASE)) { this.op[0].updateAttenuation(); this.op[1].updateAttenuation(); }
    if (change & (0xff << SHIFT_KEYCODE)) { this.op[0].updateRates(chip); this.op[1].updateRates(chip); }
  }
  updateFrequency(chip, fourOp) {
    let data = this.chanData & 0xffff;
    const kslBase = KslTable[data >> 6];
    let keyCode = (data & 0x1c00) >> 9;
    if (chip.reg08 & 0x40) keyCode |= (data & 0x100) >> 8;
    else keyCode |= (data & 0x200) >> 9;
    data |= (keyCode << SHIFT_KEYCODE) | (kslBase << SHIFT_KSLBASE);
    this.setChanData(chip, data);
    if (fourOp & 0x3f) this.next(1).setChanData(chip, data);
  }
  writeA0(chip, val) {
    const fourOp = chip.reg104 & chip.opl3Active & this.fourMask;
    if (fourOp > 0x80) return;
    const change = (this.chanData ^ val) & 0xff;
    if (change) { this.chanData ^= change; this.updateFrequency(chip, fourOp); }
  }
  writeB0(chip, val) {
    const fourOp = chip.reg104 & chip.opl3Active & this.fourMask;
    if (fourOp > 0x80) return;
    const change = (this.chanData ^ (val << 8)) & 0x1f00;
    if (change) { this.chanData ^= change; this.updateFrequency(chip, fourOp); }
    if (!((val ^ this.regB0) & 0x20)) return;
    this.regB0 = val;
    if (val & 0x20) {
      this.op[0].keyOnMask(1); this.op[1].keyOnMask(1);
      if (fourOp & 0x3f) { this.next(1).op[0].keyOnMask(1); this.next(1).op[1].keyOnMask(1); }
    } else {
      this.op[0].keyOffMask(1); this.op[1].keyOffMask(1);
      if (fourOp & 0x3f) { this.next(1).op[0].keyOffMask(1); this.next(1).op[1].keyOffMask(1); }
    }
  }
  writeC0(chip, val) {
    const change = val ^ this.regC0;
    if (!change) return;
    this.regC0 = val;
    this.feedback = (val >> 1) & 7;
    this.feedback = this.feedback ? 9 - this.feedback : 31;
    if (chip.opl3Active) {
      if ((chip.reg104 & this.fourMask) & 0x3f) {
        let chan0, chan1;
        if (!(this.fourMask & 0x80)) { chan0 = this; chan1 = this.next(1); }
        else { chan0 = this.next(-1); chan1 = this; }
        const synth = (chan0.regC0 & 1) | ((chan1.regC0 & 1) << 1);
        chan0.mode = [sm3FMFM, sm3AMFM, sm3FMAM, sm3AMAM][synth];
      } else if ((this.fourMask & 0x40) && (chip.regBD & 0x20)) {
        // percussion: left alone
      } else if (val & 1) {
        this.mode = sm3AM;
      } else {
        this.mode = sm3FM;
      }
      this.maskLeft = (val & 0x10) ? -1 : 0;
      this.maskRight = (val & 0x20) ? -1 : 0;
    } else {
      if ((this.fourMask & 0x40) && (chip.regBD & 0x20)) {
        // percussion: left alone
      } else if (val & 1) {
        this.mode = sm2AM;
      } else {
        this.mode = sm2FM;
      }
    }
  }
  resetC0(chip) {
    const val = this.regC0;
    this.regC0 ^= 0xff;
    this.writeC0(chip, val);
  }
  generatePercussion(chip, output, at, opl3) {
    const mod0 = ((this.old0 + this.old1) >>> this.feedback) | 0;
    this.old0 = this.old1;
    this.old1 = this.Op(0).getSample(mod0);
    const mod = (this.regC0 & 1) ? 0 : this.old0;
    let sample = this.Op(1).getSample(mod);
    const noiseBit = chip.forwardNoise() & 1;
    const c2 = this.Op(2).forwardWave();
    const c5 = this.Op(5).forwardWave();
    const phaseBit = (((c2 & 0x88) ^ ((c2 << 5) & 0x80)) | ((c5 ^ (c5 << 2)) & 0x20)) ? 0x02 : 0x00;
    const hhVol = this.Op(2).forwardVolume();
    if (!(hhVol >= ENV_LIMIT)) {
      const hhIndex = (phaseBit << 8) | (0x34 << (phaseBit ^ (noiseBit << 1)));
      sample += this.Op(2).getWave(hhIndex, hhVol);
    }
    const sdVol = this.Op(3).forwardVolume();
    if (!(sdVol >= ENV_LIMIT)) {
      const sdIndex = (0x100 + (c2 & 0x100)) ^ (noiseBit << 8);
      sample += this.Op(3).getWave(sdIndex, sdVol);
    }
    sample += this.Op(4).getSample(0);
    const tcVol = this.Op(5).forwardVolume();
    if (!(tcVol >= ENV_LIMIT)) {
      const tcIndex = (1 + phaseBit) << 8;
      sample += this.Op(5).getWave(tcIndex, tcVol);
    }
    sample <<= 1;
    if (opl3) { output[at] += sample; output[at + 1] += sample; }
    else output[at] += sample;
  }
  // dbopl's BlockTemplate<mode>: samples into output from at; the next channel's number
  block(chip, samples, output, at) {
    const mode = this.mode;
    switch (mode) {
      case sm2AM: case sm3AM:
        if (this.Op(0).silent() && this.Op(1).silent()) { this.old0 = this.old1 = 0; return this.n + 1; }
        break;
      case sm2FM: case sm3FM:
        if (this.Op(1).silent()) { this.old0 = this.old1 = 0; return this.n + 1; }
        break;
      case sm3FMFM:
        if (this.Op(3).silent()) { this.old0 = this.old1 = 0; return this.n + 2; }
        break;
      case sm3AMFM:
        if (this.Op(0).silent() && this.Op(3).silent()) { this.old0 = this.old1 = 0; return this.n + 2; }
        break;
      case sm3FMAM:
        if (this.Op(1).silent() && this.Op(3).silent()) { this.old0 = this.old1 = 0; return this.n + 2; }
        break;
      case sm3AMAM:
        if (this.Op(0).silent() && this.Op(2).silent() && this.Op(3).silent()) { this.old0 = this.old1 = 0; return this.n + 2; }
        break;
    }
    this.Op(0).prepare(chip);
    this.Op(1).prepare(chip);
    if (mode > sm4Start) { this.Op(2).prepare(chip); this.Op(3).prepare(chip); }
    if (mode > sm6Start) { this.Op(4).prepare(chip); this.Op(5).prepare(chip); }
    for (let i = 0; i < samples; i++) {
      if (mode === sm2Percussion) { this.generatePercussion(chip, output, at + i, false); continue; }
      if (mode === sm3Percussion) { this.generatePercussion(chip, output, at + i * 2, true); continue; }
      const mod = ((this.old0 + this.old1) >>> this.feedback) | 0;
      this.old0 = this.old1;
      this.old1 = this.Op(0).getSample(mod);
      let sample = 0, next;
      const out0 = this.old0;
      if (mode === sm2AM || mode === sm3AM) {
        sample = out0 + this.Op(1).getSample(0);
      } else if (mode === sm2FM || mode === sm3FM) {
        sample = this.Op(1).getSample(out0);
      } else if (mode === sm3FMFM) {
        next = this.Op(1).getSample(out0);
        next = this.Op(2).getSample(next);
        sample = this.Op(3).getSample(next);
      } else if (mode === sm3AMFM) {
        sample = out0;
        next = this.Op(1).getSample(0);
        next = this.Op(2).getSample(next);
        sample += this.Op(3).getSample(next);
      } else if (mode === sm3FMAM) {
        sample = this.Op(1).getSample(out0);
        next = this.Op(2).getSample(0);
        sample += this.Op(3).getSample(next);
      } else if (mode === sm3AMAM) {
        sample = out0;
        next = this.Op(1).getSample(0);
        sample += this.Op(2).getSample(next);
        sample += this.Op(3).getSample(0);
      }
      if (mode === sm2AM || mode === sm2FM) {
        output[at + i] += sample;
      } else {
        output[at + i * 2] += sample & this.maskLeft;
        output[at + i * 2 + 1] += sample & this.maskRight;
      }
    }
    if (mode <= sm3FM) return this.n + 1;
    if (mode <= sm3AMAM) return this.n + 2;
    return this.n + 3;
  }
}

export class OplChip {
  constructor(rate) {
    this.lfoCounter = 0; this.lfoAdd = 0;
    this.noiseCounter = 0; this.noiseAdd = 0; this.noiseValue = 0;
    this.freqMul = new Uint32Array(16);
    this.linearRates = new Uint32Array(76);
    this.attackRates = new Uint32Array(76);
    this.chan = [];
    for (let i = 0; i < 18; i++) this.chan.push(new Channel(this, i));
    this.reg104 = 0; this.reg08 = 0; this.reg04 = 0; this.regBD = 0;
    this.vibratoIndex = 0; this.tremoloIndex = 0; this.vibratoSign = 0; this.vibratoShift = 0;
    this.tremoloValue = 0; this.vibratoStrength = 0; this.tremoloStrength = 0;
    this.waveFormMask = 0; this.opl3Active = 0;
    this.rate = rate;
    this.setup(rate);
  }

  forwardNoise() {
    this.noiseCounter = (this.noiseCounter + this.noiseAdd) >>> 0;
    let count = this.noiseCounter >>> LFO_SH;
    this.noiseCounter &= WAVE_MASK;
    for (; count > 0; --count) {
      this.noiseValue = (this.noiseValue ^ (0x800302 & (0 - (this.noiseValue & 1)))) >>> 0;
      this.noiseValue >>>= 1;
    }
    return this.noiseValue;
  }

  forwardLFO(samples) {
    const v = VibratoTable[this.vibratoIndex >> 2];
    this.vibratoSign = v >> 7;
    this.vibratoShift = ((v & 7) + this.vibratoStrength) & 0xff;
    this.tremoloValue = TremoloTable[this.tremoloIndex] >> this.tremoloStrength;
    const todo = (LFO_MAX - this.lfoCounter) >>> 0;
    let count = Math.floor((todo + this.lfoAdd - 1) / this.lfoAdd);
    if (count > samples) {
      count = samples;
      this.lfoCounter = (this.lfoCounter + count * this.lfoAdd) >>> 0;
    } else {
      this.lfoCounter = (this.lfoCounter + count * this.lfoAdd) >>> 0;
      this.lfoCounter &= LFO_MAX - 1;
      this.vibratoIndex = (this.vibratoIndex + 1) & 31;
      if (this.tremoloIndex + 1 < TREMOLO_TABLE) ++this.tremoloIndex;
      else this.tremoloIndex = 0;
    }
    return count;
  }

  writeBD(val) {
    const change = this.regBD ^ val;
    if (!change) return;
    this.regBD = val;
    this.vibratoStrength = (val & 0x40) ? 0x00 : 0x01;
    this.tremoloStrength = (val & 0x80) ? 0x00 : 0x02;
    const c = this.chan;
    if (val & 0x20) {
      if (change & 0x20) c[6].mode = this.opl3Active ? sm3Percussion : sm2Percussion;
      if (val & 0x10) { c[6].op[0].keyOnMask(2); c[6].op[1].keyOnMask(2); }
      else { c[6].op[0].keyOffMask(2); c[6].op[1].keyOffMask(2); }
      if (val & 0x1) c[7].op[0].keyOnMask(2); else c[7].op[0].keyOffMask(2);
      if (val & 0x8) c[7].op[1].keyOnMask(2); else c[7].op[1].keyOffMask(2);
      if (val & 0x4) c[8].op[0].keyOnMask(2); else c[8].op[0].keyOffMask(2);
      if (val & 0x2) c[8].op[1].keyOnMask(2); else c[8].op[1].keyOffMask(2);
    } else if (change & 0x20) {
      c[6].resetC0(this);
      c[6].op[0].keyOffMask(2); c[6].op[1].keyOffMask(2);
      c[7].op[0].keyOffMask(2); c[7].op[1].keyOffMask(2);
      c[8].op[0].keyOffMask(2); c[8].op[1].keyOffMask(2);
    }
  }

  // A register: 0x00-0xff, and 0x100-0x1ff for OPL3's second bank
  write(reg, val) {
    val &= 0xff;
    let index, ch;
    const regop = fn => {
      index = ((reg >> 3) & 0x20) | (reg & 0x1f);
      if (OpChan[index] >= 0) this.chan[OpChan[index]].op[OpNum[index]][fn](this, val);
    };
    const regchan = fn => {
      index = ((reg >> 4) & 0x10) | (reg & 0xf);
      if (ChanIndex[index] >= 0) this.chan[ChanIndex[index]][fn](this, val);
    };
    switch ((reg & 0xf0) >> 4) {
      case 0x0:
        if (reg === 0x01) {
          this.waveFormMask = (val & 0x20) ? 0x7 : 0x0;
        } else if (reg === 0x104) {
          if (!((this.reg104 ^ val) & 0x3f)) return;
          this.reg104 = 0x80 | (val & 0x3f);
        } else if (reg === 0x105) {
          if (!((this.opl3Active ^ val) & 1)) return;
          this.opl3Active = (val & 1) ? 0xff : 0;
          for (let i = 0; i < 18; i++) this.chan[i].resetC0(this);
        } else if (reg === 0x08) {
          this.reg08 = val;
        }
        // falls through
      case 0x1:
        break;
      case 0x2: case 0x3: regop('write20'); break;
      case 0x4: case 0x5: regop('write40'); break;
      case 0x6: case 0x7: regop('write60'); break;
      case 0x8: case 0x9: regop('write80'); break;
      case 0xa: regchan('writeA0'); break;
      case 0xb:
        if (reg === 0xbd) this.writeBD(val);
        else regchan('writeB0');
        break;
      case 0xc: regchan('writeC0');
        // falls through
      case 0xd:
        break;
      case 0xe: case 0xf: regop('writeE0'); break;
    }
  }

  // total samples into output (an Int32Array): OPL2's mono, or OPL3's
  // stereo pairs once register 0x105 has turned it on
  generate(total, output) {
    const stereo = !!this.opl3Active, nch = stereo ? 18 : 9, w = stereo ? 2 : 1;
    let at = 0;
    while (total > 0) {
      const samples = this.forwardLFO(total);
      output.fill(0, at, at + samples * w);
      for (let ch = 0; ch < nch;) ch = this.chan[ch].block(this, samples, output, at);
      total -= samples;
      at += samples * w;
    }
  }

  setup(rate) {
    const scale = OPLRATE / rate;
    this.noiseAdd = Math.floor(0.5 + scale * (1 << LFO_SH)) >>> 0;
    this.noiseCounter = 0;
    this.noiseValue = 1;
    this.lfoAdd = Math.floor(0.5 + scale * (1 << LFO_SH)) >>> 0;
    this.lfoCounter = 0;
    this.vibratoIndex = 0;
    this.tremoloIndex = 0;
    const freqScale = Math.floor(0.5 + scale * (1 << (WAVE_SH - 1 - 10))) >>> 0;
    for (let i = 0; i < 16; i++) this.freqMul[i] = Math.imul(freqScale, FreqCreateTable[i]) >>> 0;
    for (let i = 0; i < 76; i++) {
      const [index, shift] = envelopeSelect(i);
      this.linearRates[i] = Math.trunc(scale * (EnvelopeIncreaseTable[index] << (RATE_SH - shift - 3))) >>> 0;
    }
    for (let i = 0; i < 62; i++) {
      const [index, shift] = envelopeSelect(i);
      const original = Math.trunc((AttackSamplesTable[index] << shift) / scale) | 0;
      let guessAdd = Math.trunc(scale * (EnvelopeIncreaseTable[index] << (RATE_SH - shift - 3))) | 0;
      let bestAdd = guessAdd;
      let bestDiff = 1 << 30;
      for (let passes = 0; passes < 16; passes++) {
        let volume = ENV_MAX, samples = 0, count = 0;
        while (volume > 0 && samples < original * 2) {
          count = (count + guessAdd) >>> 0;
          const change = (count >>> RATE_SH) | 0;
          count = (count & RATE_MASK) >>> 0;
          if (change) volume += Math.imul(~volume, change) >> 3;
          samples++;
        }
        const diff = original - samples;
        const lDiff = Math.abs(diff);
        if (lDiff < bestDiff) {
          bestDiff = lDiff;
          bestAdd = guessAdd;
          if (!bestDiff) break;
        }
        if (diff < 0) {
          const mul = (((original - diff) << 12) / original) | 0;
          guessAdd = Math.imul(guessAdd, mul) >> 12;
          guessAdd++;
        } else if (diff > 0) {
          const mul = (((original - diff) << 12) / original) | 0;
          guessAdd = Math.imul(guessAdd, mul) >> 12;
          guessAdd--;
        }
      }
      this.attackRates[i] = bestAdd >>> 0;
    }
    for (let i = 62; i < 76; i++) this.attackRates[i] = (8 << RATE_SH) >>> 0;
    const c = this.chan;
    c[0].fourMask = 0x00 | 1; c[1].fourMask = 0x80 | 1;
    c[2].fourMask = 0x00 | 2; c[3].fourMask = 0x80 | 2;
    c[4].fourMask = 0x00 | 4; c[5].fourMask = 0x80 | 4;
    c[9].fourMask = 0x00 | 8; c[10].fourMask = 0x80 | 8;
    c[11].fourMask = 0x00 | 16; c[12].fourMask = 0x80 | 16;
    c[13].fourMask = 0x00 | 32; c[14].fourMask = 0x80 | 32;
    c[6].fourMask = 0x40; c[7].fourMask = 0x40; c[8].fourMask = 0x40;
    // clear everything in OPL3 mode, then in OPL2 mode
    this.write(0x105, 0x1);
    for (let i = 0; i < 512; i++) {
      if (i === 0x105) continue;
      this.write(i, 0xff);
      this.write(i, 0x0);
    }
    this.write(0x105, 0x0);
    for (let i = 0; i < 255; i++) {
      this.write(i, 0xff);
      this.write(i, 0x0);
    }
  }
}

// snd-mix.js - the sound device's events made into samples: the AdLib
// (opl.js), eight channels of 8-bit samples, and the PC speaker, mixed to
// stereo. The page's AudioWorklet (snd-worklet.js) runs one in real time;
// cli.js --wav runs one over a whole session and writes the file.
//
// Events are snd-device.js's, [kind, t, ...] with t in microseconds of
// the device's clock. frameOf() puts t on the output's frames:
//   frame = t * rate / 1e6 + offset
// Offline the offset is 0. Live (opts.latency), every batch says what
// the device's clock read when it was sent, and the offset is set so
// that moment plays latency seconds from now -- and set again whenever
// the two clocks have drifted further apart than slack, as they do when
// a tab sleeps or the machine stalls. So a late batch never plays its
// sounds bunched up, and the music cannot creep away from the game.

import { OplChip } from './opl.js';
import { EVT } from './snd-device.js';

const VOICES = 8;
const BLOCK = 1024;
const OPL_GAIN = 4 / 32768;        // dbopl's output, scaled as Wolf4SDL scales it, to [-1, 1]
const PCM_GAIN = 0.8 / 128;
const SPK_AMP = 0.15;
const PCM_STALE = 0.2;             // seconds: a sound this late is dropped, not started

export class SoundMixer {
  constructor(rate, opts = {}) {
    this.rate = rate;
    this.opl = new OplChip(rate);
    this.obuf = new Int32Array(2 * BLOCK);
    this.voices = [];
    for (let i = 0; i < VOICES; i++) this.voices.push({ data: null, pos: 0, step: 0, vl: 0, vr: 0 });
    this.spkFreq = 0;
    this.spkPhase = 0;
    this.queue = [];
    this.head = 0;
    this.offset = 0;
    this.frame = 0;                // frames rendered so far
    this.latency = opts.latency ? Math.round(opts.latency * rate) : 0;
    this.slack = Math.round((opts.slack || 0.05) * rate);
    this.synced = false;
    this.resyncs = 0;
  }

  frameOf(t) { return Math.round(t * this.rate / 1e6 + this.offset); }

  // A batch, and (live) the device's clock when it was sent. Kept in time
  // order: OPL writes are stamped ahead, at the stream's cursor, and the
  // rest at now, so a batch's sounds can fall before its music.
  push(events, nowUs) {
    if (this.latency && nowUs !== undefined) {
      const want = this.frame + this.latency;
      const got = nowUs * this.rate / 1e6 + this.offset;
      if (!this.synced || Math.abs(got - want) > this.slack) {
        this.offset = want - nowUs * this.rate / 1e6;
        this.synced = true;
        this.resyncs++;
      }
    }
    const q = this.queue;
    for (const e of events) {
      let j = q.length;
      while (j > this.head && q[j - 1][1] > e[1]) j--;
      if (j === q.length) q.push(e); else q.splice(j, 0, e);
    }
  }

  // the next event's frame, or Infinity
  nextFrame() { return this.head < this.queue.length ? this.frameOf(this.queue[this.head][1]) : Infinity; }

  vol(v, x) { v.vl = (x & 0xff) / 255; v.vr = ((x >> 8) & 0xff) / 255; }

  apply(e) {
    switch (e[0]) {
      case EVT.OPL: this.opl.write(e[2], e[3]); break;
      case EVT.PCM: {
        if (this.frameOf(e[1]) < this.frame - PCM_STALE * this.rate) break;
        const v = this.voices[e[2] & 7];
        v.data = e[5]; v.pos = 0; v.step = e[3] / this.rate;
        this.vol(v, e[4]);
        break;
      }
      case EVT.STOP:
        if (e[2] < 0) { for (const v of this.voices) v.data = null; }
        else this.voices[e[2] & 7].data = null;
        break;
      case EVT.VOL: this.vol(this.voices[e[2] & 7], e[3]); break;
      case EVT.SPK: this.spkFreq = e[2] > 0 ? 1193180 / e[2] : 0; break;
      case EVT.RESET:
        this.opl = new OplChip(this.rate);
        for (const v of this.voices) v.data = null;
        this.spkFreq = 0;
        break;
    }
  }

  // n frames (at most BLOCK) of stereo into L and R from index at
  segment(L, R, at, n) {
    const ob = this.obuf, stereo = !!this.opl.opl3Active;
    this.opl.generate(n, ob);
    if (stereo) for (let i = 0; i < n; i++) { L[at + i] = ob[i * 2] * OPL_GAIN; R[at + i] = ob[i * 2 + 1] * OPL_GAIN; }
    else for (let i = 0; i < n; i++) L[at + i] = R[at + i] = ob[i] * OPL_GAIN;
    for (const v of this.voices) {
      if (!v.data) continue;
      const d = v.data, len = d.length;
      let pos = v.pos;
      for (let i = 0; i < n; i++) {
        const k = pos | 0;
        if (k >= len) { v.data = null; break; }
        const f = pos - k, a = d[k] - 128, b = (k + 1 < len ? d[k + 1] : 128) - 128;
        const s = (a + (b - a) * f) * PCM_GAIN;
        L[at + i] += s * v.vl; R[at + i] += s * v.vr;
        pos += v.step;
      }
      v.pos = pos;
    }
    if (this.spkFreq > 0) {
      const inc = this.spkFreq / this.rate;
      let ph = this.spkPhase;
      for (let i = 0; i < n; i++) {
        const s = ph < 0.5 ? SPK_AMP : -SPK_AMP;
        L[at + i] += s; R[at + i] += s;
        ph += inc; if (ph >= 1) ph -= 1;
      }
      this.spkPhase = ph;
    }
    for (let i = at; i < at + n; i++) {
      const l = L[i], r = R[i];
      L[i] = l > 1 ? 1 : l < -1 ? -1 : l;
      R[i] = r > 1 ? 1 : r < -1 ? -1 : r;
    }
  }

  // The next n frames: each event applied at its frame, between segments
  render(L, R, n) {
    let at = 0;
    while (at < n) {
      const nf = this.nextFrame() - this.frame;
      if (nf <= 0) { this.apply(this.queue[this.head++]); continue; }
      const k = Math.min(n - at, nf, BLOCK);
      this.segment(L, R, at, k);
      at += k;
      this.frame += k;
    }
    if (this.head > 4096) { this.queue = this.queue.slice(this.head); this.head = 0; }
  }
}

// A session's events as a 16-bit stereo WAV file (cli.js --wav), endUs
// long: the events in the device's clock, from 0
export function renderWav(events, rate, endUs) {
  const mix = new SoundMixer(rate);
  mix.push(events);
  const frames = Math.max(0, Math.ceil(endUs * rate / 1e6));
  const L = new Float32Array(4096), R = new Float32Array(4096);
  const out = new Uint8Array(44 + frames * 4);
  const dv = new DataView(out.buffer);
  const str = (at, s) => { for (let i = 0; i < s.length; i++) out[at + i] = s.charCodeAt(i); };
  str(0, 'RIFF'); dv.setUint32(4, 36 + frames * 4, true); str(8, 'WAVE');
  str(12, 'fmt '); dv.setUint32(16, 16, true); dv.setUint16(20, 1, true); dv.setUint16(22, 2, true);
  dv.setUint32(24, rate, true); dv.setUint32(28, rate * 4, true); dv.setUint16(32, 4, true); dv.setUint16(34, 16, true);
  str(36, 'data'); dv.setUint32(40, frames * 4, true);
  for (let f = 0; f < frames; f += 4096) {
    const n = Math.min(4096, frames - f);
    mix.render(L, R, n);
    for (let i = 0; i < n; i++) {
      dv.setInt16(44 + (f + i) * 4, Math.round(L[i] * 32767), true);
      dv.setInt16(44 + (f + i) * 4 + 2, Math.round(R[i] * 32767), true);
    }
  }
  return out;
}

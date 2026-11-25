// snd-gm.js - the General MIDI synth behind the sound card's MIDI port
// (snd-device.js): SpessaSynth's processor (vendor/spessasynth_core.js,
// Apache-2.0, vendor/README.md) playing the person's own SoundFont (.sf2,
// .sf3) or DLS, which the page finds in the Uploads (uploads.js). The
// mixer (snd-mix.js) hands it each message at the frame it is stamped for
// and has it add its samples into the mix, so the music keeps the same
// time as the AdLib's would, and the card's MASTER scales it too.
//
// It runs where the mixer runs: the page's AudioWorklet, where it cannot
// be import()ed late, so the page adds this module to the worklet ahead
// of snd-worklet.js, and it leaves GmSynth on globalThis for the worklet
// to find (page.js, SoundOut); the main thread's mixer, on a page with
// no AudioWorklet; and cli.js --sf2, writing a WAV.
//
// Until its SoundFont is in, it keeps what sets the channels up (programs,
// controllers, pitch bends, system exclusives) and lets notes go, then
// plays what it kept: a song that started while the SoundFont was being
// read comes in with its instruments.

import * as lib from './vendor/spessasynth_core.js';

const QUANTUM = 128;                 // the most the processor renders at once
const KEEP = 4096;                   // set-up messages kept while loading
// SpessaSynth's mix for a SoundFont's notes at full velocity is around a
// quarter of full scale, a fair match for the AdLib music (OPL_GAIN)
const GM_GAIN = 1;

// A game's MIDI has controllers and system exclusives no SoundFont synth
// knows: quiet, rather than a warning for each in the audio thread
lib.SpessaLog.setLogLevel(false, false, false);

export class GmSynth {
  constructor(rate) {
    this.rate = rate;
    this.proc = new lib.SpessaSynthProcessor(rate, { effectsEnabled: true, eventsEnabled: false });
    // no SoundFont yet: quiet, not a warning for every program change
    this.proc.onMissingPreset = () => undefined;
    this.ready = false;
    this.failed = null;
    this.name = '';
    this.presets = 0;
    this.messages = 0;
    this.kept = [];
    this.bufL = new Float32Array(QUANTUM);
    this.bufR = new Float32Array(QUANTUM);
  }

  // A SoundFont's or DLS's bytes (an ArrayBuffer or a Uint8Array); resolves
  // when it plays, or rejects with why it cannot
  async load(bytes, name = '') {
    try {
      const buf = bytes instanceof ArrayBuffer ? bytes : bytes.buffer.slice(bytes.byteOffset, bytes.byteOffset + bytes.byteLength);
      const bank = lib.SoundBankLoader.fromArrayBuffer(buf);
      this.proc.soundBankManager.addSoundBank(bank, 'main');
      await this.proc.ready;
      this.name = name;
      this.presets = bank.presets.length;
      this.ready = true;
      const kept = this.kept;
      this.kept = [];
      for (const m of kept) this.proc.processMessage(m);
    } catch (e) {
      this.failed = String(e && e.message || e);
      this.kept = [];
      throw e;
    }
  }

  midi(msg) {
    this.messages++;
    if (this.ready) { this.proc.processMessage(msg); return; }
    if (this.failed) return;
    const k = msg[0] & 0xf0;
    if (k === 0x80 || k === 0x90 || k === 0xa0) return;
    this.kept.push(msg);
    if (this.kept.length > KEEP) this.kept.splice(0, KEEP >> 1);
  }

  // the card's RESET: every note stopped, every channel as it starts
  reset() {
    this.kept = [];
    this.proc.stopAll(true);
    this.proc.reset();
  }

  // n frames added into L and R from index at
  render(L, R, at, n) {
    if (!this.ready) return;
    const bl = this.bufL, br = this.bufR;
    for (let i = 0; i < n; i += QUANTUM) {
      const k = Math.min(QUANTUM, n - i);
      bl.fill(0, 0, k); br.fill(0, 0, k);
      this.proc.process(bl, br, 0, k);
      for (let j = 0; j < k; j++) { L[at + i + j] += bl[j] * GM_GAIN; R[at + i + j] += br[j] * GM_GAIN; }
    }
  }

  voices() { return this.proc.voiceCount; }
}

// a tiny SoundFont of SpessaSynth's own (one saw wave), for the tests
export const sampleSoundFont = () => lib.BasicSoundBank.getSampleSoundBankFile();

// for snd-worklet.js, which cannot import() it (above)
globalThis.C4GmSynth = GmSynth;

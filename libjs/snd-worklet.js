// snd-worklet.js - the sound device's mixer (snd-mix.js) in the page's
// audio thread. page.js loads it with audioWorklet.addModule and posts
// it each batch of events the Worker sends: { events, now }.
//
// For General MIDI music page.js adds snd-gm.js to the worklet first and
// posts { soundfont, name }: the SoundFont's bytes, which a GmSynth
// (globalThis.C4GmSynth) loads, and the mixer then plays the card's MIDI
// port with. It answers { gm: 'ready', name, presets } or { gm: 'failed',
// why }.

import { SoundMixer } from './snd-mix.js';

class C4Sound extends AudioWorkletProcessor {
  constructor() {
    super();
    // 60 ms behind the device: a Worker slice is 16, and a batch has to
    // cross from the Worker to the page to here
    this.mix = new SoundMixer(sampleRate, { latency: 0.06, slack: 0.05 });
    this.port.onmessage = e => {
      const m = e.data;
      if (m.soundfont) this.soundfont(m.soundfont, m.name || '');
      if (m.events) this.mix.push(m.events, m.now);
      if (m.stats) {
        const s = this.mix.synth;
        this.port.postMessage({
          frame: this.mix.frame, resyncs: this.mix.resyncs, queued: this.mix.queue.length - this.mix.head,
          gm: s ? { ready: s.ready, failed: s.failed, name: s.name, presets: s.presets, messages: s.messages, voices: s.voices(), peak: this.peak } : null,
        });
        this.peak = 0;
      }
    };
    this.peak = 0;                 // the loudest sample since the last stats, for the tests
  }

  soundfont(bytes, name) {
    const Gm = globalThis.C4GmSynth;
    if (!Gm) { this.port.postMessage({ gm: 'failed', why: 'snd-gm.js is not in the worklet' }); return; }
    const synth = this.mix.synth || (this.mix.synth = new Gm(sampleRate));
    synth.load(bytes, name).then(
      () => this.port.postMessage({ gm: 'ready', name, presets: synth.presets }),
      err => this.port.postMessage({ gm: 'failed', why: String(err && err.message || err) }));
  }

  process(inputs, outputs) {
    const out = outputs[0];
    const L = out[0], R = out[1] || out[0];
    this.mix.render(L, R, L.length);
    let p = this.peak;
    for (let i = 0; i < L.length; i++) { const a = Math.abs(L[i]); if (a > p) p = a; }
    this.peak = p;
    return true;
  }
}

registerProcessor('c4-sound', C4Sound);

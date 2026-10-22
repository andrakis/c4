// snd-worklet.js - the sound device's mixer (snd-mix.js) in the page's
// audio thread. page.js loads it with audioWorklet.addModule and posts
// it each batch of events the Worker sends: { events, now }.

import { SoundMixer } from './snd-mix.js';

class C4Sound extends AudioWorkletProcessor {
  constructor() {
    super();
    // 60 ms behind the device: a Worker slice is 16, and a batch has to
    // cross from the Worker to the page to here
    this.mix = new SoundMixer(sampleRate, { latency: 0.06, slack: 0.05 });
    this.port.onmessage = e => {
      const m = e.data;
      if (m.events) this.mix.push(m.events, m.now);
      if (m.stats) this.port.postMessage({ frame: this.mix.frame, resyncs: this.mix.resyncs, queued: this.mix.queue.length - this.mix.head });
    };
  }

  process(inputs, outputs) {
    const out = outputs[0];
    const L = out[0], R = out[1] || out[0];
    this.mix.render(L, R, L.length);
    return true;
  }
}

registerProcessor('c4-sound', C4Sound);

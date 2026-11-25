// page.js - the machine, from the page.
//
//   import { C4M } from '/libjs/page.js';
//   const vm = await C4M.boot({
//     image: 'src/c4bb/images/c4ix32.c4r',     // a URL, or bytes
//     disk:  'src/c4bb/images/disk',           // a directory with a manifest.json, or a Map
//     argv:  ['c4ix32.c4r'],
//     terminal: document.getElementById('terminal'),   // optional
//     display:  document.getElementById('display'),    // optional <canvas>
//     snd:      true,                                   // optional: a sound card, played here
//     persist:  'c4ix',                                 // optional: drive 1, kept in IndexedDB under this name
//     uploads:  'uploads',                              // optional: files laid over drive 0, kept in IndexedDB under this name
//     uploadsHome: true,                                // optional: ... over drive 1 instead, C4DOS's B: (uploads.js HomeFiles)
//     music:    'gm',                                   // optional: 'opl' (default), 'gm' (a SoundFont from the uploads), 'midi' (Web MIDI)
//     soundfont: 'soundfonts/gm.sf2',                   // optional: which upload is the SoundFont (default: the first)
//   });
//   (a lazy disk's files with a hash, once read, are kept in IndexedDB too: lazy.js; cache: false keeps none)
//   vm.on('out', bytes => ...);   vm.on('status', s => ...);   vm.on('exit', e => ...)
//   vm.on('music', m => ...)      // how the music plays: { music: 'opl' | 'gm' | 'midi', name, why }
//   vm.write('ps\n');  vm.signal(2);  vm.pause();  vm.resume();  vm.reset();
//   await vm.resetFiles()       // forget drive 1 (opts.persist), and boot again
//   await vm.peek(addr, len);  await vm.poke(addr, bytes)
//
// The machine runs in a Worker (worker.js); everything here is messages.
// URLs resolve against the page, and the Worker's imports need the repo
// root to be what is served, as for c4bb.

import { Terminal } from '../src/c4bb/web/panels.js';
import { Display } from './display.js';
import { makeShared } from './shared.js';
import { MediaStore, indexedDbBackend } from '../src/c4bb/web/store.js';
import { cacheMedium, loadCache, keepInCache } from './lazy.js';
import { pickSoundFont } from './uploads.js';

async function fetchBytes(url) {
  const r = await fetch(url);
  if (!r.ok) throw new Error(`${url}: ${r.status}`);
  return new Uint8Array(await r.arrayBuffer());
}

// A disk is a directory with manifest.json listing its files (what
// src/c4bb/tests/build-images.sh writes), fetched whole, a few at a time.
async function fetchDisk(dir, onProgress) {
  const names = await (await fetch(`${dir}/manifest.json`)).json();
  const files = [];
  let next = 0, done = 0;
  const worker = async () => {
    while (next < names.length) {
      const name = names[next++];
      files.push([name, await fetchBytes(`${dir}/${name}`)]);
      done++;
      if (onProgress) onProgress(done, names.length);
    }
  };
  await Promise.all(Array.from({ length: 8 }, worker));
  return files;
}

// A lazy disk: index.json names every file and its size, and nothing
// is fetched up front -- the worker reads a file from the server the
// first time the machine opens it (LazyFiles in worker.js). A disk
// with no index.json falls back to fetchDisk. opts.lazy === false
// forces the old way.
async function fetchIndex(dir) {
  try {
    const r = await fetch(`${dir}/index.json`);
    if (!r.ok) return null;
    return await r.json();
  } catch { return null; }
}

// The sound card's events (snd-device.js), played by an AudioWorklet
// (snd-worklet.js, which mixes them with snd-mix.js). A page that is not
// a secure context -- plain http from another host -- has no
// AudioWorklet, and there the same mixer runs on this thread, in a
// ScriptProcessorNode, a little further behind. A browser starts an
// AudioContext suspended until the page is used, so the first key or
// click resumes it. Until it runs nothing is sent, but the OPL's
// registers are kept, and sent when it starts with every note off, so
// music that was already playing comes in with its instruments; and so
// is the master volume, which the desktop sets as it starts.
//
// General MIDI (opts.music): the card's MIDI port plays either through a
// SoundFont synth beside the mixer (snd-gm.js: this.gm is the SoundFont,
// sent to the worklet as it starts, and what sets the channels up is kept
// while it cannot play, as the OPL's registers are), or through Web MIDI
// to a synth outside the browser (this.out: Microsoft GS Wavetable Synth
// on Windows, as DOSBox uses there), sent with the time each message is
// due, whether or not the AudioContext runs.
const MIDI_KEEP = 2048;                   // set-up messages kept while nothing plays
const isNote = m => (m[0] & 0xf0) === 0x80 || (m[0] & 0xf0) === 0x90 || (m[0] & 0xf0) === 0xa0;
class SoundOut {
  constructor() {
    this.ctx = null; this.failed = false; this.how = null;
    this.send = null;                     // a batch to the mixer, once there is one
    this.ask = null;                      // the mixer's stats
    this.shadow = new Map();
    this.master = null;                   // the card's MASTER, set while nothing was sent
    this.gm = null;                       // { name, bytes }: the SoundFont, for the mixer's synth
    this.gmState = null;                  // what the synth said: { gm: 'ready' | 'failed', ... }
    this.onGm = null;
    this.out = null;                      // a Web MIDI output
    this.midiKept = [];
    this.outMaster = 255;                 // the card's MASTER, as Web MIDI hears it (channel volumes)
    this.outVol = new Array(16).fill(100);
  }

  start() {
    if (this.ctx || this.failed) return;
    try { this.ctx = new AudioContext({ latencyHint: 'interactive' }); }
    catch { this.failed = true; return; }
    const resume = () => { if (this.ctx.state !== 'running') this.ctx.resume().catch(() => {}); };
    for (const ev of ['keydown', 'pointerdown']) addEventListener(ev, resume, { capture: true });
    if (!this.ctx.audioWorklet) { this.here(); return; }
    const wl = this.ctx.audioWorklet;
    (this.gm ? wl.addModule(new URL('./snd-gm.js', import.meta.url)) : Promise.resolve()).then(
      () => wl.addModule(new URL('./snd-worklet.js', import.meta.url))).then(() => {
      const node = new AudioWorkletNode(this.ctx, 'c4-sound', { numberOfInputs: 0, outputChannelCount: [2] });
      node.connect(this.ctx.destination);
      this.how = 'worklet';
      this.send = (m, transfer) => node.port.postMessage(m, transfer);
      let asked = [];
      node.port.onmessage = e => {
        if (typeof e.data.gm === 'string') this.gmSaid(e.data);
        else { const a = asked; asked = []; for (const res of a) res(e.data); }
      };
      this.ask = () => new Promise(res => { asked.push(res); node.port.postMessage({ stats: 1 }); });
      if (this.gm) {
        const b = this.gm.bytes;
        this.send({ soundfont: b.buffer.slice(b.byteOffset, b.byteOffset + b.byteLength), name: this.gm.name });
      }
    }, () => this.here());
  }

  gmSaid(m) {
    this.gmState = m;
    if (this.onGm) this.onGm(m);
  }

  // the mixer on this thread
  here() {
    Promise.all([import('./snd-mix.js'), this.gm ? import('./snd-gm.js') : null]).then(([{ SoundMixer }, gmMod]) => {
      let synth = null;
      if (gmMod) {
        synth = new gmMod.GmSynth(this.ctx.sampleRate);
        synth.load(this.gm.bytes, this.gm.name).then(() => this.gmSaid({ gm: 'ready', name: this.gm.name, presets: synth.presets }),
          err => this.gmSaid({ gm: 'failed', why: String(err && err.message || err) }));
      }
      const mix = new SoundMixer(this.ctx.sampleRate, { latency: 0.12, slack: 0.08, synth });
      const sp = this.ctx.createScriptProcessor(2048, 0, 2);
      sp.onaudioprocess = e => {
        const out = e.outputBuffer;
        mix.render(out.getChannelData(0), out.getChannelData(1), out.length);
      };
      sp.connect(this.ctx.destination);
      this.how = 'main thread';
      this.send = m => { if (m.events) mix.push(m.events, m.now); };
      this.ask = () => Promise.resolve({ frame: mix.frame, resyncs: mix.resyncs, queued: mix.queue.length - mix.head, opl: mix.oplState(),
        gm: synth ? { ready: synth.ready, failed: synth.failed, name: synth.name, presets: synth.presets, messages: synth.messages, voices: synth.voices() } : null });
    }, () => { this.failed = true; });
  }

  // Web MIDI: the card's MIDI messages to this.out, each at its time on
  // this page's clock -- the batch's `now` is the device's clock as it was
  // sent -- as far behind as the mixer plays. Channel volumes are scaled
  // by the card's MASTER, which is the mix's and so never reaches the
  // synth otherwise. No system exclusives: Web MIDI asks the person
  // separately for those. The rest of the batch is returned, for the mixer.
  toOut(events, now) {
    const o = this.out, base = performance.now() + 60 + (this.ctx ? (this.ctx.baseLatency + (this.ctx.outputLatency || 0)) * 1000 : 0);
    const vol = v => Math.round(v * (this.outMaster & 0x100 ? 0 : this.outMaster & 0xff) / 255);
    const rest = [];
    for (const e of events) {
      const at = base + (e[1] - now) / 1000;
      try {
        if (e[0] === 8) {
          const m = e[2];
          if (m[0] >= 0xf0) continue;
          if ((m[0] & 0xf0) === 0xb0 && m[1] === 7) { this.outVol[m[0] & 15] = m[2]; o.send([m[0], 7, vol(m[2])], at); }
          else o.send(m, at);
          continue;
        }
        if (e[0] === 6) for (let ch = 0; ch < 16; ch++) { o.send([0xb0 | ch, 120, 0], at); o.send([0xb0 | ch, 121, 0], at); this.outVol[ch] = 100; }
        if (e[0] === 7) { this.outMaster = e[2]; for (let ch = 0; ch < 16; ch++) o.send([0xb0 | ch, 7, vol(this.outVol[ch])], at); }
      } catch { /* a closed port, or a message it will not take */ }
      rest.push(e);
    }
    return rest;
  }

  // every note off, now: the page is going
  quiet() {
    if (!this.out) return;
    try { for (let ch = 0; ch < 16; ch++) this.out.send([0xb0 | ch, 120, 0]); } catch { /* closed */ }
  }

  play(events, now) {
    if (this.out) events = this.toOut(events, now);
    this.start();
    if (!this.send || this.ctx.state !== 'running') {
      for (const e of events) {
        if (e[0] === 1) this.shadow.set(e[2], e[3]);
        else if (e[0] === 6) { this.shadow.clear(); this.midiKept = []; }
        else if (e[0] === 7) this.master = e[2];
        else if (e[0] === 8 && !isNote(e[2])) {
          this.midiKept.push(e[2]);
          if (this.midiKept.length > MIDI_KEEP) this.midiKept.splice(0, MIDI_KEEP >> 1);
        }
      }
      return;
    }
    if (this.midiKept.length) { events = this.midiKept.map(m => [8, now, m]).concat(events); this.midiKept = []; }
    if (this.master !== null) { events = [[7, now, this.master]].concat(events); this.master = null; }
    if (this.shadow.size) {
      const regs = [];
      for (const [reg, val] of this.shadow) {
        const r = reg & 0xff;
        regs.push([1, now, reg, r >= 0xb0 && r <= 0xb8 ? val & ~0x20 : r === 0xbd ? val & ~0x1f : val]);
      }
      this.shadow.clear();
      events = regs.concat(events);
    }
    const transfer = [];
    for (const e of events) if (e[5] instanceof Uint8Array) transfer.push(e[5].buffer);
    this.send({ events, now }, transfer);
  }

  // what the mixer has played -- { frame, resyncs, queued, opl, state, how } -- or
  // null. opl is snd-mix.js oplState(): window.c4m.sound.stats() in the
  // console, when the music is silent, says whether the OPL is written at
  // all (writes), plays anything (rms, peak) and in which mode (opl3, c0)
  async stats() {
    if (!this.ask) return null;
    return { ...(await this.ask()), state: this.ctx.state, how: this.how };
  }
}

export class C4M {
  static async boot(opts) {
    const vm = new C4M(opts);
    await vm.start(opts);
    return vm;
  }

  constructor(opts = {}) {
    this.worker = new Worker(new URL('./worker.js', import.meta.url), { type: 'module' });
    this.listeners = new Map();
    this.replies = new Map();
    this.nextId = 1;
    this.status = { state: 'loading' };
    this.raw = false;
    this.exited = null;
    this.sound = opts.snd ? new SoundOut() : null;
    this.music = { music: 'opl' };
    this.worker.onmessage = e => this.receive(e.data);
    this.worker.onerror = e => this.emit('error', { message: e.message || 'worker failed to start' });
    if (opts.terminal) this.attachTerminal(opts.terminal);
    if (opts.display) this.attachDisplay(opts.display);
  }

  async start(opts) {
    this.emit('status', { state: 'loading' });
    const image = opts.image instanceof Uint8Array ? opts.image : await fetchBytes(opts.image);
    let disk = [], lazy = null;
    if (opts.disk instanceof Map) disk = [...opts.disk];
    else if (opts.disk && opts.lazy !== false && (lazy = await fetchIndex(opts.disk))) {
      lazy = { base: new URL(opts.disk + '/', location.href).href, index: lazy };
    } else if (opts.disk) disk = await fetchDisk(opts.disk, (n, of) => this.emit('loading', { n, of }));
    const argv = opts.argv || [String(opts.image).split('/').pop()];
    // Drive 1: what the machine keeps there (C4IX's persist) is kept in
    // IndexedDB as c4bb's media are (src/c4bb/web/store.js), and given
    // back at the next boot. opts.persistReset forgets it first. A page
    // with no IndexedDB (some private windows) simply has no drive 1.
    let persist = null;
    if (opts.persist) {
      try {
        this.store = this.store || new MediaStore(indexedDbBackend());
        if (opts.persistReset) await this.store.erase(opts.persist);
        persist = { files: [...(await this.store.load(opts.persist))] };
        this.persistName = opts.persist;
      } catch { persist = null; }
    }
    // Uploads: files the person gave the page (app.js's Uploads), kept in
    // IndexedDB the same way and laid over drive 0 at every boot, so a
    // program opens them by name as if the disk had them.
    let uploads = [];
    if (opts.uploads) {
      try {
        this.store = this.store || new MediaStore(indexedDbBackend());
        uploads = [...(await this.store.load(opts.uploads))];
      } catch { uploads = []; }
    }
    const midi = this.sound ? await this.musicFor(opts, uploads) : false;
    // The shared display path, when the page is cross-origin isolated and
    // has a display; opts.shared === false asks for the message path.
    if (this.display && opts.shared !== false && globalThis.crossOriginIsolated && !this.shared) {
      this.shared = makeShared();
      this.display.attachShared(this.shared);
    }
    this.bootMsg = {
      type: 'boot', image: image.buffer.slice(image.byteOffset, image.byteOffset + image.byteLength),
      argv, disk: disk.map(([n, b]) => [n, b]), lazy, arenaMb: opts.arenaMb || 64,
      mhz: opts.mhz || 20, unpaced: !!opts.unpaced, realtime: !!opts.realtime, mbox: opts.mbox | 0, fpu: opts.fpu, accel: opts.accel, osAccel: opts.osAccel,
      cpus: opts.cpus | 0, quantum: opts.quantum | 0, mmu: !!opts.mmu,
      // threads need SharedArrayBuffer: a cross-origin isolated page
      parallel: !!opts.parallel && !!globalThis.crossOriginIsolated,
      gui: this.display ? { w: this.display.width, h: this.display.height } : (opts.gui || null),
      snd: !!this.sound,
      midi,
      persist,
      uploads,
      // C4DOS has them on B:, its home drive, rather than on A: (uploads.js)
      uploadsHome: !!opts.uploadsHome,
      shared: this.shared || null,
    };
    this.worker.postMessage(this.bootMsg);
    // The lazy disk's files that are worth keeping (index.json gives them
    // a hash: the games' sources) are kept in IndexedDB once read, and
    // what is there goes to the worker as the machine boots, which does
    // not wait for it: a file opened before it arrives is fetched
    // (lazy.js). Never with opts.cache === false.
    if (lazy && opts.cache !== false && lazy.index.some(e => e[2])) this.loadCache(opts.disk, lazy.index);
  }

  // How the music plays (opts.music), decided before the machine boots,
  // since the card says so (CAPS bit 5) and a game asks it once: 'gm' needs
  // a SoundFont among the uploads; 'midi' needs Web MIDI and an output,
  // and falls back to 'gm', then to the AdLib. True for General MIDI.
  async musicFor(opts, uploads) {
    const want = opts.music === 'gm' || opts.music === 'midi' ? opts.music : 'opl';
    const sf = want === 'opl' ? null : pickSoundFont(uploads, opts.soundfont);
    let why = '';
    if (want === 'midi' && !this.sound.out) {
      try {
        if (!navigator.requestMIDIAccess) throw new Error('this browser has no Web MIDI');
        const access = await navigator.requestMIDIAccess();
        const outs = [...access.outputs.values()];
        const o = outs.find(x => opts.midiOut && x.name.includes(opts.midiOut)) || outs[0];
        if (!o) throw new Error('no MIDI output');
        await o.open();
        this.sound.out = o;
        addEventListener('pagehide', () => this.sound.quiet());
      } catch (e) { why = `Web MIDI: ${e && e.message || e}`; }
    }
    if (want === 'midi' && this.sound.out) this.music = { music: 'midi', name: this.sound.out.name };
    else if (want !== 'opl' && sf) {
      this.sound.gm = { name: sf[0], bytes: sf[1] instanceof Uint8Array ? sf[1] : new Uint8Array(sf[1]) };
      this.music = { music: 'gm', name: sf[0], why };
      this.sound.onGm = m => {
        this.music = m.gm === 'ready' ? { music: 'gm', name: m.name, presets: m.presets } : { music: 'gm', name: sf[0], why: m.why };
        this.emit('music', this.music);
      };
    } else this.music = { music: 'opl', why: want === 'opl' ? '' : why || 'no SoundFont (.sf2, .sf3, .dls) in the Uploads' };
    this.emit('music', this.music);
    return this.music.music !== 'opl';
  }

  async loadCache(disk, index) {
    try {
      this.store = this.store || new MediaStore(indexedDbBackend());
      this.cacheName = cacheMedium(disk);
      const files = await loadCache(this.store.backend, this.cacheName, index);
      if (files.length) this.worker.postMessage({ type: 'cache', files });
      this.emit('cache', { loaded: files.length });
    } catch { this.cacheName = null; }
  }

  on(ev, fn) {
    if (!this.listeners.has(ev)) this.listeners.set(ev, new Set());
    this.listeners.get(ev).add(fn);
    return () => this.listeners.get(ev).delete(fn);
  }
  emit(ev, arg) { for (const fn of this.listeners.get(ev) || []) fn(arg); }

  receive(m) {
    switch (m.type) {
      case 'out': this.emit('out', m.bytes); break;
      case 'status': this.status = m; this.emit('status', m); break;
      case 'kbd': this.raw = m.raw; this.emit('kbd', m); break;
      case 'display': this.emit('display', m.frames); break;
      case 'snd': if (this.sound) this.sound.play(m.events, m.now); break;
      case 'persist':
        // dropped while a Reset Files is under way: the old machine's
        // (bytes null: the machine deleted it)
        if (this.store && !this.persistHold) (m.bytes ? this.store.backend.putFile(this.persistName, m.name, m.bytes)
          : this.store.backend.deleteFile(this.persistName, m.name)).then(
          () => this.emit('persist', { name: m.name, bytes: m.bytes ? m.bytes.length : -1 }), () => {});
        break;
      case 'cache':
        if (this.cacheName) keepInCache(this.store.backend, this.cacheName, m.name, m.hash, m.bytes).then(
          () => this.emit('cache', { name: m.name, bytes: m.bytes.length }), () => {});
        break;
      // a file the machine handed the person (C4IX's download): the page saves it
      case 'download': this.emit('download', { name: m.name, bytes: m.bytes }); break;
      case 'gui': this.emit('resize', m); break;
      case 'exit': this.exited = m; this.emit('exit', m); break;
      case 'error': this.emit('error', m); break;
      case 'files-reset':
        this.persistHold = false;
        if (this.filesReset) { const r = this.filesReset; this.filesReset = null; r(true); }
        break;
      case 'reply': {
        const r = this.replies.get(m.id);
        if (r) { this.replies.delete(m.id); r(m); }
        break;
      }
    }
  }

  request(msg, transfer = []) {
    const id = this.nextId++;
    return new Promise(res => { this.replies.set(id, res); this.worker.postMessage({ ...msg, id }, transfer); });
  }

  // ---- input -------------------------------------------------------
  write(s) {
    const bytes = typeof s === 'string' ? new TextEncoder().encode(s) : new Uint8Array(s);
    this.worker.postMessage({ type: 'input', bytes }, [bytes.buffer]);
  }
  key(e) {
    this.worker.postMessage({ type: 'key', key: e.key, ctrlKey: !!e.ctrlKey, altKey: !!e.altKey, metaKey: !!e.metaKey });
  }
  // Text typed as keys, a line at a time as the machine reads them
  // (keys.js Paste): what the terminal's Paste does, for any system.
  paste(text) { if (text) this.worker.postMessage({ type: 'paste', text: String(text) }); }
  // The clipboard, pasted. It needs a click or a key behind it (a user
  // gesture) and a secure context; where the browser will not read it,
  // or the person said no, a box to paste into instead (pasteBox).
  async pasteClipboard() {
    try {
      if (!navigator.clipboard || !navigator.clipboard.readText) throw new Error('no clipboard');
      this.paste(await navigator.clipboard.readText());
      if (this.termEl) this.termEl.focus();
    } catch {
      this.pasteBox();
    }
  }
  pasteBox() {
    const el = this.termEl;
    if (!el || typeof document === 'undefined') return;
    const d = document.createElement('dialog');
    d.className = 'c4-paste-box';
    d.innerHTML = '<form method="dialog"><h2>Paste</h2>' +
      '<p class="dim">The browser would not let the page read the clipboard. Paste here (Ctrl+V), then Type.</p>' +
      '<textarea rows="8" cols="60" spellcheck="false"></textarea>' +
      '<div class="config-buttons"><span class="grow"></span><button value="cancel">Cancel</button>' +
      '<button value="ok" class="primary">Type</button></div></form>';
    document.body.append(d);
    const ta = d.querySelector('textarea');
    d.addEventListener('close', () => {
      if (d.returnValue === 'ok') this.paste(ta.value);
      d.remove();
      el.focus();
    });
    d.showModal();
    ta.focus();
  }
  signal(sig = 2) { this.worker.postMessage({ type: 'signal', sig }); }
  eof() { this.worker.postMessage({ type: 'eof' }); }
  guiEvent(ev) { this.worker.postMessage({ type: 'gui', ...ev }); }

  // ---- control -----------------------------------------------------
  pause() { this.worker.postMessage({ type: 'pause' }); }
  resume() { this.worker.postMessage({ type: 'resume' }); }
  reset() {
    this.exited = null;
    if (this.term) this.term.clear();
    if (this.display) { this.display.discard(); this.display.clear(); }
    this.worker.postMessage({ type: 'reset' });
  }
  // Forget what drive 1 kept -- every change C4IX saved to its files --
  // and boot again from the disk as it ships. IndexedDB is emptied first;
  // then the worker empties the drive and boots in one step, so nothing
  // the old machine does can follow it. A file it sent before that is
  // dropped here (persistHold) until the worker says the new one is up.
  async resetFiles() {
    if (!this.store) return false;
    this.persistHold = true;
    await this.store.erase(this.persistName);
    this.exited = null;
    if (this.term) this.term.clear();
    if (this.display) { this.display.discard(); this.display.clear(); }
    return new Promise(res => { this.filesReset = res; this.worker.postMessage({ type: 'reset-files' }); });
  }
  terminate() { this.worker.terminate(); }

  // ---- memory ------------------------------------------------------
  async peek(addr, len) { return (await this.request({ type: 'peek', addr, len })).bytes; }
  async poke(addr, bytes) {
    const b = new Uint8Array(bytes);
    return (await this.request({ type: 'poke', addr, bytes: b }, [b.buffer])).ok;
  }

  // ---- the terminal ------------------------------------------------
  // c4bb's Terminal (a screen with a cursor, CSI and SGR) fed from 'out',
  // with keydowns sent to the Worker, which decides cooked or raw.
  //
  // Copy and paste, as a terminal does them:
  //   Ctrl+Shift+V, Shift+Insert, and Ctrl+V while no program has the
  //   keyboard raw (to a shell Ctrl+V is nothing; a full-screen program
  //   gets it): paste, through the browser's own paste event, which needs
  //   no permission, or the clipboard if no event comes.
  //   Ctrl+Shift+C, Ctrl+Insert, and Ctrl+C with text selected in the
  //   terminal: copy (Ctrl+C with nothing selected is still SIGINT).
  //   The right button: a menu with Copy and Paste.
  attachTerminal(el) {
    this.term = new Terminal(el, 25, 80);
    this.termEl = el;
    this.on('out', bytes => { for (const b of bytes) this.term.write(b); });
    const selected = () => {
      const sel = typeof getSelection === 'function' ? getSelection() : null;
      return sel && !sel.isCollapsed && el.contains(sel.anchorNode) ? sel.toString() : '';
    };
    const copy = () => {
      const t = selected();
      if (t && navigator.clipboard) navigator.clipboard.writeText(t).catch(() => document.execCommand('copy'));
      else if (t) document.execCommand('copy');
      return !!t;
    };
    let waitPaste = 0;
    el.addEventListener('keydown', e => {
      const k = e.key.length === 1 ? e.key.toLowerCase() : e.key;
      const pasteKey = (e.ctrlKey && e.shiftKey && k === 'v') || (e.shiftKey && !e.ctrlKey && k === 'Insert') ||
        (e.ctrlKey && !e.shiftKey && !e.altKey && k === 'v' && !this.raw);
      if (pasteKey && !e.metaKey) {
        // the browser's paste event comes next, with the text; if it
        // does not (some browsers send none to an element that is not
        // editable), read the clipboard, the key being the gesture
        clearTimeout(waitPaste);
        waitPaste = setTimeout(() => { waitPaste = 0; this.pasteClipboard(); }, 100);
        return;
      }
      if (!e.metaKey && ((e.ctrlKey && e.shiftKey && k === 'c') || (e.ctrlKey && !e.shiftKey && k === 'Insert') ||
          (e.ctrlKey && !e.shiftKey && !e.altKey && k === 'c' && selected()))) {
        copy();
        e.preventDefault();
        return;
      }
      // Keys the browser would otherwise act on belong to the machine
      // while the terminal has focus. Meta combinations (copy, paste,
      // reload) are left alone.
      if (e.metaKey) return;
      if (e.key.length === 1 || e.ctrlKey || e.altKey ||
          ['Enter', 'Backspace', 'Tab', 'Escape', 'ArrowUp', 'ArrowDown', 'ArrowLeft',
           'ArrowRight', 'Home', 'End', 'PageUp', 'PageDown', 'Insert', 'Delete'].includes(e.key) ||
          /^F\d+$/.test(e.key)) {
        this.key(e);
        e.preventDefault();
      }
    });
    el.addEventListener('paste', e => {
      clearTimeout(waitPaste);
      waitPaste = 0;
      const text = e.clipboardData && e.clipboardData.getData('text');
      e.preventDefault();
      if (text) this.paste(text);
    });
    el.addEventListener('contextmenu', e => {
      e.preventDefault();
      this.termMenu(e.clientX, e.clientY, selected(), copy);
    });
    return this.term;
  }

  // The terminal's right-click menu: Copy (when text is selected) and Paste.
  termMenu(x, y, sel, copy) {
    if (this.menuEl) this.menuEl.remove();
    const m = document.createElement('div');
    m.className = 'c4-term-menu';
    m.setAttribute('role', 'menu');
    const item = (label, keys, enabled, act) => {
      const b = document.createElement('button');
      b.type = 'button';
      b.setAttribute('role', 'menuitem');
      b.disabled = !enabled;
      b.innerHTML = `<span>${label}</span><span class="dim">${keys}</span>`;
      b.addEventListener('click', () => { close(); act(); });
      m.append(b);
    };
    const close = () => {
      m.remove();
      if (this.menuEl === m) this.menuEl = null;
      removeEventListener('pointerdown', away, true);
      removeEventListener('keydown', esc, true);
      removeEventListener('blur', close);
    };
    const away = ev => { if (!m.contains(ev.target)) close(); };
    const esc = ev => { if (ev.key === 'Escape') { ev.preventDefault(); ev.stopPropagation(); close(); this.termEl.focus(); } };
    item('Copy', 'Ctrl+Shift+C', !!sel, () => { copy(); this.termEl.focus(); });
    item('Paste', 'Ctrl+Shift+V', true, () => this.pasteClipboard());
    document.body.append(m);
    const r = m.getBoundingClientRect();
    m.style.left = Math.max(0, Math.min(x, innerWidth - r.width - 2)) + 'px';
    m.style.top = Math.max(0, Math.min(y, innerHeight - r.height - 2)) + 'px';
    this.menuEl = m;
    addEventListener('pointerdown', away, true);
    addEventListener('keydown', esc, true);
    addEventListener('blur', close);
  }

  // ---- the display -------------------------------------------------
  attachDisplay(canvas) {
    this.display = new Display(canvas, ev => this.guiEvent(ev));
    this.on('display', frames => this.display.draw(frames));
    this.on('resize', ({ w, h }) => this.display.resize(w, h));
    return this.display;
  }
}

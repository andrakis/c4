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
//   });
//   vm.on('out', bytes => ...);   vm.on('status', s => ...);   vm.on('exit', e => ...)
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
// music that was already playing comes in with its instruments.
class SoundOut {
  constructor() {
    this.ctx = null; this.failed = false; this.how = null;
    this.send = null;                     // a batch to the mixer, once there is one
    this.ask = null;                      // the mixer's stats
    this.shadow = new Map();
  }

  start() {
    if (this.ctx || this.failed) return;
    try { this.ctx = new AudioContext({ latencyHint: 'interactive' }); }
    catch { this.failed = true; return; }
    const resume = () => { if (this.ctx.state !== 'running') this.ctx.resume().catch(() => {}); };
    for (const ev of ['keydown', 'pointerdown']) addEventListener(ev, resume, { capture: true });
    if (!this.ctx.audioWorklet) { this.here(); return; }
    this.ctx.audioWorklet.addModule(new URL('./snd-worklet.js', import.meta.url)).then(() => {
      const node = new AudioWorkletNode(this.ctx, 'c4-sound', { numberOfInputs: 0, outputChannelCount: [2] });
      node.connect(this.ctx.destination);
      this.how = 'worklet';
      this.send = (m, transfer) => node.port.postMessage(m, transfer);
      this.ask = () => new Promise(res => {
        node.port.onmessage = e => res(e.data);
        node.port.postMessage({ stats: 1 });
      });
    }, () => this.here());
  }

  // the mixer on this thread
  here() {
    import('./snd-mix.js').then(({ SoundMixer }) => {
      const mix = new SoundMixer(this.ctx.sampleRate, { latency: 0.12, slack: 0.08 });
      const sp = this.ctx.createScriptProcessor(2048, 0, 2);
      sp.onaudioprocess = e => {
        const out = e.outputBuffer;
        mix.render(out.getChannelData(0), out.getChannelData(1), out.length);
      };
      sp.connect(this.ctx.destination);
      this.how = 'main thread';
      this.send = m => { if (m.events) mix.push(m.events, m.now); };
      this.ask = () => Promise.resolve({ frame: mix.frame, resyncs: mix.resyncs, queued: mix.queue.length - mix.head });
    }, () => { this.failed = true; });
  }

  play(events, now) {
    this.start();
    if (!this.send || this.ctx.state !== 'running') {
      for (const e of events) {
        if (e[0] === 1) this.shadow.set(e[2], e[3]);
        else if (e[0] === 6) this.shadow.clear();
      }
      return;
    }
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

  // what the mixer has played -- { frame, resyncs, queued, state, how } -- or null
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
    // The shared display path, when the page is cross-origin isolated and
    // has a display; opts.shared === false asks for the message path.
    if (this.display && opts.shared !== false && globalThis.crossOriginIsolated && !this.shared) {
      this.shared = makeShared();
      this.display.attachShared(this.shared);
    }
    this.bootMsg = {
      type: 'boot', image: image.buffer.slice(image.byteOffset, image.byteOffset + image.byteLength),
      argv, disk: disk.map(([n, b]) => [n, b]), lazy, arenaMb: opts.arenaMb || 64,
      mhz: opts.mhz || 20, unpaced: !!opts.unpaced, realtime: !!opts.realtime, mbox: opts.mbox | 0, fpu: opts.fpu,
      cpus: opts.cpus | 0, quantum: opts.quantum | 0,
      // threads need SharedArrayBuffer: a cross-origin isolated page
      parallel: !!opts.parallel && !!globalThis.crossOriginIsolated,
      gui: this.display ? { w: this.display.width, h: this.display.height } : (opts.gui || null),
      snd: !!this.sound,
      persist,
      shared: this.shared || null,
    };
    this.worker.postMessage(this.bootMsg);
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
        if (this.store && !this.persistHold) this.store.backend.putFile(this.persistName, m.name, m.bytes).then(
          () => this.emit('persist', { name: m.name, bytes: m.bytes.length }), () => {});
        break;
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
  attachTerminal(el) {
    this.term = new Terminal(el, 25, 80);
    this.on('out', bytes => { for (const b of bytes) this.term.write(b); });
    el.addEventListener('keydown', e => {
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
      const text = e.clipboardData.getData('text');
      if (text) { this.write(text); e.preventDefault(); }
    });
    return this.term;
  }

  // ---- the display -------------------------------------------------
  attachDisplay(canvas) {
    this.display = new Display(canvas, ev => this.guiEvent(ev));
    this.on('display', frames => this.display.draw(frames));
    this.on('resize', ({ w, h }) => this.display.resize(w, h));
    return this.display;
  }
}

// page.js - the machine, from the page.
//
//   import { C4M } from '/libjs/page.js';
//   const vm = await C4M.boot({
//     image: 'src/c4bb/images/c4ix32.c4r',     // a URL, or bytes
//     disk:  'src/c4bb/images/disk',           // a directory with a manifest.json, or a Map
//     argv:  ['c4ix32.c4r'],
//     terminal: document.getElementById('terminal'),   // optional
//     display:  document.getElementById('display'),    // optional <canvas>
//   });
//   vm.on('out', bytes => ...);   vm.on('status', s => ...);   vm.on('exit', e => ...)
//   vm.write('ps\n');  vm.signal(2);  vm.pause();  vm.resume();  vm.reset();
//   await vm.peek(addr, len);  await vm.poke(addr, bytes)
//
// The machine runs in a Worker (worker.js); everything here is messages.
// URLs resolve against the page, and the Worker's imports need the repo
// root to be what is served, as for c4bb.

import { Terminal } from '../src/c4bb/web/panels.js';
import { Display } from './display.js';

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
    this.worker.onmessage = e => this.receive(e.data);
    this.worker.onerror = e => this.emit('error', { message: e.message || 'worker failed to start' });
    if (opts.terminal) this.attachTerminal(opts.terminal);
    if (opts.display) this.attachDisplay(opts.display);
  }

  async start(opts) {
    this.emit('status', { state: 'loading' });
    const image = opts.image instanceof Uint8Array ? opts.image : await fetchBytes(opts.image);
    let disk = [];
    if (opts.disk instanceof Map) disk = [...opts.disk];
    else if (opts.disk) disk = await fetchDisk(opts.disk, (n, of) => this.emit('loading', { n, of }));
    const argv = opts.argv || [String(opts.image).split('/').pop()];
    this.bootMsg = {
      type: 'boot', image: image.buffer.slice(image.byteOffset, image.byteOffset + image.byteLength),
      argv, disk: disk.map(([n, b]) => [n, b]), arenaMb: opts.arenaMb || 64,
      mhz: opts.mhz || 20, unpaced: !!opts.unpaced, mbox: opts.mbox | 0,
      gui: this.display ? { w: this.display.width, h: this.display.height } : (opts.gui || null),
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
      case 'gui': this.emit('resize', m); break;
      case 'exit': this.exited = m; this.emit('exit', m); break;
      case 'error': this.emit('error', m); break;
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
    if (this.display) this.display.clear();
    this.worker.postMessage({ type: 'reset' });
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

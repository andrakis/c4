// display.js - the machine's display, on the page.
//
// The Worker ships batches of command frames ([len, type, ...payload],
// gui-device.js); this replays them onto an offscreen canvas and copies
// it to the visible one on PRESENT, so a half-drawn frame is never seen.
// A guest that never sends PRESENT is drawing in immediate mode, and
// every batch is shown as it arrives.
//
// The other direction: pointer, wheel and key events on the canvas
// become event frames for the guest. Mouse moves are coalesced to one
// per animation frame.
//
// Colours are 0x00RRGGBB. In an image (IMGDEF) the top byte is
// transparency: 0 is opaque, 255 is fully transparent.

import { CMD } from './gui-device.js';
import { RingReader, FbConsumer } from './shared.js';

const css = rgb => '#' + ((rgb >>> 0) & 0xffffff).toString(16).padStart(6, '0');

export class Display {
  constructor(canvas, send) {
    this.canvas = canvas;
    this.send = send;
    this.width = canvas.width;
    this.height = canvas.height;
    this.back = document.createElement('canvas');
    this.back.width = this.width;
    this.back.height = this.height;
    this.ctx = this.back.getContext('2d');
    this.front = canvas.getContext('2d');
    this.images = new Map();
    this.clipped = false;
    this.presentMode = false;            // true once the guest has sent a PRESENT
    this.framesDrawn = 0;
    this.commandsDrawn = 0;
    this.recordText = false; this.texts = []; this.shownText = [];
    this.clear();
    this.listen();
  }

  clear() {
    this.ctx.fillStyle = '#000';
    this.ctx.fillRect(0, 0, this.width, this.height);
    this.front.fillStyle = '#000';
    this.front.fillRect(0, 0, this.width, this.height);
    this.presentMode = false;
  }

  resize(w, h) {
    if (w === this.width && h === this.height) return;
    this.width = this.canvas.width = this.back.width = w;
    this.height = this.canvas.height = this.back.height = h;
    this.clipped = false;
    this.clear();
  }

  // The shared path (shared.js): commands come out of a ring in shared
  // memory, read once per animation frame, and framebuffer frames out of
  // a triple buffer. Nothing is posted; the page draws at display rate.
  attachShared({ ring, fb }) {
    this.ring = new RingReader(ring);
    this.fb = new FbConsumer(fb);
    this.fbGen = -1;
    this.shared = true;
    const pump = () => {
      const words = this.ring.read();
      if (words) this.drawCoalesced(words);
    };
    const loop = () => { pump(); requestAnimationFrame(loop); };
    requestAnimationFrame(loop);
    // A hidden page gets no animation frames. Keep emptying the ring
    // anyway, more slowly, so the Worker never has to hold commands back
    // (or, eventually, drop them) because nobody is looking; the message
    // path draws in a hidden tab too.
    setInterval(() => { if (document.hidden) pump(); }, 200);
  }

  // One screen refresh's worth of commands, drawn once. Everything before
  // the last full-screen frame (FB or FBREF) is covered by it, so only the
  // commands that change state (SIZE, IMGDEF, CLIP, NOCLIP) are replayed
  // from before it; the rest is drawn from that frame on, and shown with
  // one present at the end instead of one per PRESENT. What the page shows
  // after the batch is exactly what drawing every command would show.
  drawCoalesced(words) {
    let last = -1;
    for (let i = 0; i < words.length && words[i] >= 2; i += words[i]) {
      const t = words[i + 1];
      if (t === CMD.FB || t === CMD.FBREF) last = i;
    }
    this.coalescing = true;
    if (last > 0) {
      const keep = [];
      for (let i = 0; i < last; i += words[i]) {
        const t = words[i + 1];
        if (t === CMD.SIZE || t === CMD.IMGDEF || t === CMD.CLIP || t === CMD.NOCLIP)
          for (let k = 0; k < words[i]; k++) keep.push(words[i + k]);
      }
      if (keep.length) this.draw(Int32Array.from(keep));
      this.draw(words.subarray(last));
    } else this.draw(words);
    this.coalescing = false;
    this.present();
  }

  // Throw away whatever is waiting in the ring (a reset).
  discard() { if (this.ring) this.ring.read(); }

  // Draw w x h pixels of 0x00RRGGBB, scaled to the display. Converted to
  // RGBA only when they are new (gen), so a frame drawn twice costs one
  // drawImage the second time.
  // hm is the height word from the device: h | (mode << 16). In
  // column-major mode (1) the pixels are w columns of h, which is an image
  // h wide and w tall; a transform that swaps the axes draws it upright.
  blitPixels(pixels, w, hm, gen) {
    const mode = hm >>> 16, h = hm & 0xffff;
    const iw = mode ? h : w, ih = mode ? w : h;
    if (!this.fbCanvas || this.fbCanvas.width !== iw || this.fbCanvas.height !== ih) {
      this.fbCanvas = document.createElement('canvas');
      this.fbCanvas.width = iw; this.fbCanvas.height = ih;
      this.fbImage = new ImageData(iw, ih);
      this.fbGen = -1;
    }
    if (gen === undefined || gen !== this.fbGen) {
      const px = new Uint32Array(this.fbImage.data.buffer);
      for (let k = 0; k < w * h; k++) {
        const v = pixels[k];
        // 0x00RRGGBB in, RGBA bytes out (little-endian: ABGR in a word)
        px[k] = 0xff000000 | ((v & 0xff) << 16) | (v & 0xff00) | ((v >> 16) & 0xff);
      }
      this.fbCanvas.getContext('2d').putImageData(this.fbImage, 0, 0);
      if (gen !== undefined) this.fbGen = gen;
    }
    this.ctx.imageSmoothingEnabled = false;
    if (mode) {
      // source (u across, v down) -> display (x = v, y = u), scaled
      this.ctx.setTransform(0, this.height / h, this.width / w, 0, 0, 0);
      this.ctx.drawImage(this.fbCanvas, 0, 0);
      this.ctx.setTransform(1, 0, 0, 1, 0, 0);
    } else this.ctx.drawImage(this.fbCanvas, 0, 0, this.width, this.height);
    this.framesBlitted = (this.framesBlitted | 0) + 1;
    this.presentMode = true;
    this.present();
  }

  present() {
    if (this.coalescing) return;          // drawCoalesced presents once at the end
    this.front.drawImage(this.back, 0, 0);
    this.framesDrawn++;
    if (this.recordText) this.shownText = this.texts.slice();
  }

  // For tests: with recordText on, shownText is every string the last
  // presented frame drew, as { x, y, s }. A canvas cannot be read back as
  // text, and this is the next best thing to reading the screen.
  noteText(x, y, s) { if (this.recordText) this.texts.push({ x, y, s }); }

  draw(words) {
    const c = this.ctx;
    let i = 0;
    while (i < words.length) {
      const len = words[i], type = words[i + 1], p = i + 2;
      if (len < 2 || i + len > words.length) break;
      this.commandsDrawn++;
      switch (type) {
        case CMD.CLEAR:
          c.fillStyle = css(words[p]); c.fillRect(0, 0, this.width, this.height);
          this.texts = [];
          break;
        case CMD.RECT:
          c.fillStyle = css(words[p + 4]); c.fillRect(words[p], words[p + 1], words[p + 2], words[p + 3]); break;
        case CMD.RECTO:
          c.strokeStyle = css(words[p + 4]); c.lineWidth = 1;
          c.strokeRect(words[p] + 0.5, words[p + 1] + 0.5, words[p + 2] - 1, words[p + 3] - 1); break;
        case CMD.LINE:
          c.strokeStyle = css(words[p + 4]); c.lineWidth = 1;
          c.beginPath(); c.moveTo(words[p] + 0.5, words[p + 1] + 0.5);
          c.lineTo(words[p + 2] + 0.5, words[p + 3] + 0.5); c.stroke(); break;
        case CMD.CIRCLE:
          c.beginPath(); c.arc(words[p], words[p + 1], Math.max(0, words[p + 2]), 0, Math.PI * 2);
          if (words[p + 4]) { c.fillStyle = css(words[p + 3]); c.fill(); }
          else { c.strokeStyle = css(words[p + 3]); c.lineWidth = 1; c.stroke(); }
          break;
        case CMD.TEXT: {
          const n = Math.max(0, Math.min(words[p + 4], (len - 7) * 4));
          let s = '';
          for (let k = 0; k < n; k++) s += String.fromCharCode((words[p + 5 + (k >> 2)] >>> ((k & 3) * 8)) & 0xff);
          c.fillStyle = css(words[p + 2]);
          c.font = `${Math.max(4, words[p + 3])}px ui-monospace, "JetBrains Mono", monospace`;
          c.textBaseline = 'top';
          c.fillText(s, words[p], words[p + 1]);
          this.noteText(words[p], words[p + 1], s);
          break;
        }
        case CMD.TEXT2: {
          // Text with a face and, for a character grid, a fixed advance:
          // each character exactly `advance` pixels after the last.
          const n = Math.max(0, Math.min(words[p + 6], (len - 9) * 4));
          const font = words[p + 4], adv = words[p + 5];
          c.fillStyle = css(words[p + 2]);
          c.font = `${font === 2 ? 'bold ' : ''}${Math.max(4, words[p + 3])}px ` +
                   (font ? 'Tahoma, "Segoe UI", Verdana, sans-serif' : 'ui-monospace, Consolas, "DejaVu Sans Mono", monospace');
          c.textBaseline = 'top';
          let s = '';
          for (let k = 0; k < n; k++) s += String.fromCharCode((words[p + 7 + (k >> 2)] >>> ((k & 3) * 8)) & 0xff);
          this.noteText(words[p], words[p + 1], s);
          if (!adv) c.fillText(s, words[p], words[p + 1]);
          else for (let k = 0; k < n; k++) if (s[k] !== ' ') c.fillText(s[k], words[p] + k * adv, words[p + 1]);
          break;
        }
        case CMD.PIXEL:
          c.fillStyle = css(words[p + 2]); c.fillRect(words[p], words[p + 1], 1, 1); break;
        case CMD.PRESENT:
          this.presentMode = true;
          this.present();
          break;
        case CMD.SIZE:
          this.resize(Math.max(1, words[p]), Math.max(1, words[p + 1])); break;
        case CMD.IMGDEF: {
          const id = words[p], w = words[p + 1], h = words[p + 2];
          if (w <= 0 || h <= 0 || w * h > len - 5) break;
          const img = new ImageData(w, h);
          for (let k = 0; k < w * h; k++) {
            const v = words[p + 3 + k];
            img.data[k * 4] = (v >> 16) & 0xff;
            img.data[k * 4 + 1] = (v >> 8) & 0xff;
            img.data[k * 4 + 2] = v & 0xff;
            img.data[k * 4 + 3] = 255 - ((v >>> 24) & 0xff);
          }
          const cv = document.createElement('canvas');
          cv.width = w; cv.height = h;
          cv.getContext('2d').putImageData(img, 0, 0);
          this.images.set(id, cv);
          break;
        }
        case CMD.IMG: {
          const cv = this.images.get(words[p]);
          if (cv) c.drawImage(cv, words[p + 1], words[p + 2]);
          break;
        }
        case CMD.CLIP:
          if (this.clipped) c.restore();
          c.save(); c.beginPath(); c.rect(words[p], words[p + 1], words[p + 2], words[p + 3]); c.clip();
          this.clipped = true;
          break;
        case CMD.FB: {
          // A whole framebuffer, carried in the stream (the message path).
          const w = words[p], hm = words[p + 1], h = hm & 0xffff;
          if (w <= 0 || h <= 0 || w * h > len - 4) break;
          this.blitPixels(words.subarray(p + 2, p + 2 + w * h), w, hm);
          break;
        }
        case CMD.FBREF: {
          // The shared path: the newest frame in the triple buffer. An
          // older marker in the same batch draws the same (newest) frame,
          // which is why pixels are only converted when they change.
          if (!this.fb) break;
          const f = this.fb.acquire();
          if (f.w > 0 && (f.h & 0xffff) > 0) this.blitPixels(f.pixels, f.w, f.h, f.gen);
          break;
        }
        case CMD.NOCLIP:
          if (this.clipped) { c.restore(); this.clipped = false; }
          break;
      }
      i += len;
    }
    if (!this.presentMode) this.present();
  }

  // ---- events --------------------------------------------------------
  listen() {
    const cv = this.canvas;
    if (!cv.hasAttribute('tabindex')) cv.tabIndex = 0;
    const at = e => {
      const r = cv.getBoundingClientRect();
      return {
        x: Math.floor((e.clientX - r.left) * this.width / Math.max(1, r.width)),
        y: Math.floor((e.clientY - r.top) * this.height / Math.max(1, r.height)),
      };
    };
    let move = null;
    const flushMove = () => { if (move) { this.send(move); move = null; } };
    cv.addEventListener('pointermove', e => {
      const first = !move;
      move = { kind: 'move', ...at(e), buttons: e.buttons | 0 };
      if (first) requestAnimationFrame(flushMove);
    });
    cv.addEventListener('pointerdown', e => {
      cv.focus();
      flushMove();
      this.send({ kind: 'down', ...at(e), button: e.button | 0 });
    });
    cv.addEventListener('pointerup', e => { flushMove(); this.send({ kind: 'up', ...at(e), button: e.button | 0 }); });
    cv.addEventListener('wheel', e => {
      this.send({ kind: 'wheel', ...at(e), dy: Math.sign(e.deltaY) | 0 });
      e.preventDefault();
    }, { passive: false });
    cv.addEventListener('contextmenu', e => e.preventDefault());
    const mods = e => (e.shiftKey ? 1 : 0) | (e.ctrlKey ? 2 : 0) | (e.altKey ? 4 : 0) | (e.metaKey ? 8 : 0);
    const key = kind => e => {
      if (e.metaKey) return;
      const ch = e.key.length === 1 ? e.key.charCodeAt(0) : (e.key === 'Enter' ? 10 : e.key === 'Backspace' ? 8 : e.key === 'Tab' ? 9 : e.key === 'Escape' ? 27 : 0);
      this.send({ kind, code: e.keyCode | 0, char: ch > 255 ? 0 : ch, mods: mods(e) });
      e.preventDefault();
    };
    cv.addEventListener('keydown', key('keydown'));
    cv.addEventListener('keyup', key('keyup'));
    cv.addEventListener('focus', () => this.send({ kind: 'focus', focus: 1 }));
    cv.addEventListener('blur', () => this.send({ kind: 'focus', focus: 0 }));
  }
}

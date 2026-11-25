// test-shared.mjs - the display's SharedArrayBuffer path (shared.js).
//
// The command ring: frames come out whole and in order, across the wrap
// and across a reader that falls behind (the writer keeps what does not
// fit and the caller puts it back). The triple buffer: the reader always
// gets the newest published frame, never a slot the writer is filling.
// Then fb-demo end to end, with the Worker's and the page's halves both
// driven here: pixels through the triple buffer, markers through the ring.

import { readFileSync } from 'node:fs';
import { makeShared, RingWriter, RingReader, FbProducer, FbConsumer } from '../shared.js';
import { createMachine, boot } from '../boot.js';
import { STOP } from '../c4m.js';
import { GuiDevice, CMD } from '../gui-device.js';

let fail = 0;
const check = (cond, what, extra = '') => {
  console.log(`test-shared: ${what} ${cond ? 'OK' : 'FAILED'}${!cond && extra ? '\n  ' + extra : ''}`);
  if (!cond) fail = 1;
};

// ---- the ring, small enough to wrap many times ------------------------
{
  const cap = 64;
  const sab = new SharedArrayBuffer((4 + cap) * 4);
  new Int32Array(sab)[2] = cap;
  // Start the counters just below 2^31 so they wrap past it too.
  new Int32Array(sab)[0] = 0x7ffffff0; new Int32Array(sab)[1] = 0x7ffffff0;
  const wr = new RingWriter(sab), rd = new RingReader(sab);
  let sent = 0, got = 0, ok = true, pending = new Int32Array(0);
  const frame = k => { const n = 2 + (k % 5); const f = new Int32Array(n); f[0] = n; f[1] = 7; for (let i = 2; i < n; i++) f[i] = k * 100 + i; return f; };
  for (let round = 0; round < 500; round++) {
    // the writer: three frames plus anything left over from before
    const parts = [pending, frame(sent), frame(sent + 1), frame(sent + 2)];
    sent += 3;
    const all = new Int32Array(parts.reduce((s, p) => s + p.length, 0));
    let at = 0; for (const p of parts) { all.set(p, at); at += p.length; }
    const n = wr.write(all);
    pending = all.slice(n);
    // the reader: only every third round, so the writer keeps running out of room
    if (round % 3 === 0 || round === 499) {
      const w = rd.read();
      for (let i = 0; w && i < w.length; i += w[i]) {
        const f = frame(got++);
        if (w[i] !== f[0]) { ok = false; break; }
        for (let k = 0; k < f.length; k++) if (w[i + k] !== f[k]) ok = false;
      }
    }
  }
  while (pending.length) { const n = wr.write(pending); pending = pending.slice(n); const w = rd.read(); for (let i = 0; w && i < w.length; i += w[i]) { const f = frame(got++); for (let k = 0; k < f.length; k++) if (w[i + k] !== f[k]) ok = false; } }
  check(ok && got === sent, `ring: ${got} frames out whole and in order, across wraps and a slow reader`);
}

// ---- the triple buffer ------------------------------------------------
{
  const { fb } = makeShared();
  const prod = new FbProducer(fb), cons = new FbConsumer(fb);
  const src = new Int32Array(64);
  let ok = true;
  const held = new Set();
  for (let k = 1; k <= 200; k++) {
    src.fill(k);
    prod.publish(src, 0, 16, 4, 4);                      // 4x4 of value k
    if (prod.back === cons.front) ok = false;            // never the slot the page holds
    if (k % 7 === 0) {
      const f = cons.acquire();
      if (f.pixels[0] !== k || f.w !== 4) ok = false;   // always the newest
      held.add(cons.front);
      if (prod.back === cons.front) ok = false;
    }
  }
  check(ok && held.size === 3, 'triple buffer: the page always gets the newest frame and never shares a slot');
  const f1 = cons.acquire(), f2 = cons.acquire();
  check(f1.gen === f2.gen, 'triple buffer: nothing new means the same frame, same generation');
}

// ---- fb-demo through the shared path -----------------------------------
{
  const shared = makeShared();
  const gui = new GuiDevice({ w: 640, h: 480, fbShared: new FbProducer(shared.fb) });
  const writer = new RingWriter(shared.ring), reader = new RingReader(shared.ring), cons = new FbConsumer(shared.fb);
  const out = [];
  const vm = createMachine({ arenaMb: 16, gui, onByte: b => out.push(b) });
  vm.dev.rxEof = true;
  boot(vm, new Uint8Array(readFileSync(new URL('../../src/c4bb/images/fb-demo.c4r', import.meta.url))), ['fb-demo.c4r', '6']);
  let refs = 0, bigFrames = 0, first = null, r;
  for (let i = 0; i < 10000; i++) {
    r = vm.run(320000);
    const f = gui.drain();
    if (f) { const n = writer.write(f); if (n < f.length) gui.putBack(f.subarray(n)); }
    const w = reader.read();
    for (let k = 0; w && k < w.length; k += w[k]) {
      if (w[k + 1] === CMD.FB) bigFrames++;
      if (w[k + 1] === CMD.FBREF) {
        refs++;
        const fr = cons.acquire();
        if (!first) first = { w: fr.w, h: fr.h, px: fr.pixels.slice(0, 320 * 240) };
      }
    }
    if (r === STOP.HALT) break;
  }
  check(r === STOP.HALT && vm.dev.status === 0, 'fb-demo runs to the end on the shared path', Buffer.from(out).toString());
  check(refs === 6 && bigFrames === 0, `every flip is a marker in the ring, no pixels in it (${refs} markers)`);
  const want = (x, y) => ((x & 255) << 16) | (((y + y) & 255) << 8) | ((x ^ y) & 255);
  check(first && first.w === 320 && first.h === 240 && first.px[20 * 320 + 10] === want(10, 20) && first.px[7 * 320 + 300] === want(300, 7),
        'the pixels arrive through the triple buffer exactly');
}
process.exit(fail);

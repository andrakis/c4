// test-paste.mjs - a paste into the page's terminal, and its cursor.
//
// A paste goes in as typed keys a line at a time, as the guest reads them
// (keys.js Paste; the worker feeds it the same way between slices). Here,
// headless, with the worker's loop in miniature:
//   - the text is made ASCII: CRLF is Enter, typographic punctuation is
//     plain, accents come off, control characters go;
//   - a full-screen program (a raw keyboard) gets Enter as CR, no echo;
//   - C4DOS (the page's DOS disk) and C4KE (the shared disk) each run a
//     pasted list of commands in order, every line echoed at the prompt
//     that reads it, not all at once, and a 40-line paste all of it.
// And the terminal (src/c4bb/web/panels.js Terminal) draws a cursor where
// the guest's is, and none while a program has hidden it (ESC[?25l).
//
//   node libjs/tests/test-paste.mjs

import { readFileSync, existsSync } from 'node:fs';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { createMachine, boot } from '../boot.js';
import { STOP } from '../c4m.js';
import { Paste, pasteText } from '../keys.js';
import { Terminal } from '../../src/c4bb/web/panels.js';

const T = 'test-paste';
let fail = 0;
const check = (cond, what, extra = '') => {
  console.log(`${T}: ${what} ${cond ? 'OK' : 'FAILED'}${!cond && extra ? '\n  ' + String(extra).slice(-1500) : ''}`);
  if (!cond) fail = 1;
  return cond;
};

// ---- the text --------------------------------------------------------------
check(pasteText('DIR\r\nVER\rMEM\n') === 'DIR\nVER\nMEM\n', 'CRLF and CR are Enter', JSON.stringify(pasteText('DIR\r\nVER\rMEM\n')));
check(pasteText('echo \u201ccaf\u00e9\u201d \u2014 it\u2019s ok\u2026') === 'echo "cafe" - it\'s ok...',
  'quotes, dashes, accents and ellipses made plain', pasteText('echo \u201ccaf\u00e9\u201d \u2014 it\u2019s ok\u2026'));
check(pasteText('a\x03b\x1bc\u00a0d\u4e2de\tf') === 'abc d?e\tf', 'control characters dropped, the rest a ?',
  JSON.stringify(pasteText('a\x03b\x1bc\u00a0d\u4e2de\tf')));

// a raw keyboard: CR for Enter, nothing echoed, a line only when asked
{
  const m = { dev: { rxFifo: [], rawKbd: 1, rxStarved: false } };
  const p = new Paste();
  p.add('ab\ncd\n');
  const e1 = p.feed(m);
  check(e1 && e1.length === 0 && m.dev.rxFifo.join() === '97,98,13', 'raw: the first line at once, Enter as CR, no echo', m.dev.rxFifo.join());
  check(p.feed(m) === null, 'raw: not the next until that one is read');
  m.dev.rxFifo.length = 0;
  check(p.feed(m) === null, 'raw: nor until the program asks again');
  m.dev.rxStarved = true;
  check(p.feed(m) && m.dev.rxFifo.join() === '99,100,13' && p.pending === 0, 'raw: then the next', m.dev.rxFifo.join());
}

// ---- booted systems --------------------------------------------------------
class LazyDisk extends Map {
  constructor(dir) {
    super();
    this.dir = dir;
    this.sizes = new Map(JSON.parse(readFileSync(join(dir, 'index.json'), 'utf8')));
  }
  has(n) { return super.has(n) || this.sizes.has(n); }
  get(n) {
    if (super.has(n)) return super.get(n);
    if (!this.sizes.has(n)) return undefined;
    const b = new Uint8Array(readFileSync(join(this.dir, n)));
    super.set(n, b);
    return b;
  }
}
// A machine with the worker's paste loop: before every slice, and when the
// guest stops for a key, the paste's next line if the guest wants it,
// echoed into the transcript as the worker echoes it to the page.
function machine(image, dir) {
  const out = [];
  const vm = createMachine({ arenaMb: 64, mhz: 400, drives: [{ files: new LazyDisk(dir), writable: false, sink: null }],
                             onByte: b => out.push(b) });
  boot(vm, new Uint8Array(readFileSync(image)), [image.replace(/.*\//, '')]);
  const paste = new Paste();
  let log = '';
  const text = () => { if (out.length) log += Buffer.from(out.splice(0)).toString('latin1'); return log; };
  const feed = () => { const e = paste.feed(vm); if (e) out.push(...e); return !!e; };
  const until = (cond, secs) => {
    const t0 = Date.now();
    while (Date.now() - t0 < secs * 1000) {
      if (cond()) return true;
      feed();
      const r = vm.run(2e6);
      if (r === STOP.HALT) break;
      if (r === STOP.INPUT) feed();
    }
    return cond();
  };
  return { vm, paste, text, until };
}

const IMG = fileURLToPath(new URL('../images/', import.meta.url));
const BB = fileURLToPath(new URL('../../src/c4bb/images/', import.meta.url));

// C4DOS, the page's DOS disk
if (existsSync(join(IMG, 'dos/index.json'))) {
  const m = machine(join(IMG, 'dos/c4dos32.c4r'), join(IMG, 'dos'));
  const A = /A:\\[^>\n]*>\s*$/;
  check(m.until(() => A.test(m.text().slice(-100)), 60), 'C4DOS: boots to A:\\>', m.text().slice(-500));
  let at = m.text().length;
  m.paste.add('ECHO first-line\r\nECHO \u201csecond\u201d \u2014 line\nVER\n');
  m.until(() => !m.paste.pending && /C4DOS/.test(m.text().slice(at)) && A.test(m.text().slice(-100)), 60);
  let o = m.text().slice(at);
  check(/^ECHO first-line\r?\nfirst-line\r?\n[\s\S]*?>\s*ECHO "second" - line\r?\n"second" - line\r?\n[\s\S]*?>\s*VER\r?\n[\s\S]*C4DOS/.test(o),
    'C4DOS: three pasted lines run in turn, each echoed at its prompt', JSON.stringify(o));
  at = m.text().length;
  let list = '';
  for (let i = 1; i <= 40; i++) list += `ECHO paste-${i}\n`;
  m.paste.add(list);
  m.until(() => !m.paste.pending && /paste-40\r?\n[\s\S]*>\s*$/.test(m.text().slice(at)), 120);
  o = m.text().slice(at);
  const got = [...o.matchAll(/^paste-(\d+)\r?$/gm)].map(x => +x[1]);
  check(got.length === 40 && got.every((n, i) => n === i + 1), `C4DOS: a 40-line paste, every line, in order (${got.length})`, o.slice(-600));
} else console.log(`${T}: C4DOS SKIPPED (no libjs/images/dos: make c4bb-images)`);

// C4KE, the shared disk
if (existsSync(join(BB, 'disk/index.json')) && existsSync(join(BB, 'c4ke32.c4r'))) {
  const m = machine(join(BB, 'c4ke32.c4r'), join(BB, 'disk'));
  const KE = /c4sh>\s*$/;
  check(m.until(() => KE.test(m.text().slice(-100)), 120), 'C4KE: boots to c4sh>', m.text().slice(-500));
  const at = m.text().length;
  m.paste.add('echo first-line\necho second-line\nhello.c4r\n');
  m.until(() => !m.paste.pending && /yello/.test(m.text().slice(at)) && KE.test(m.text().slice(-100)), 120);
  const o = m.text().slice(at);
  check(/^echo first-line\r?\nfirst-line\r?\n[\s\S]*?c4sh>\s*echo second-line\r?\nsecond-line\r?\n[\s\S]*?c4sh>\s*hello\.c4r\r?\n[\s\S]*yello/.test(o),
    'C4KE: three pasted lines run in turn, each echoed at its prompt', JSON.stringify(o));
} else console.log(`${T}: C4KE SKIPPED (no src/c4bb/images/disk: make c4bb-images)`);

// ---- the cursor ------------------------------------------------------------
{
  const t = new Terminal(null, 25, 80);
  const CUR = /<span class="c4-term-cursor[^"]*"[^>]*>([^<]*)<\/span>/;
  t.feed('Hello\r\nA:\\>');
  let h = t.toHtml();
  check(/A:\\&gt;<span class="c4-term-cursor c4-term-cursor-pad"><\/span>$/.test(h), 'the cursor after the prompt', h);
  t.feed('\x1b[1;2H');
  h = t.toHtml();
  check(CUR.exec(h) && CUR.exec(h)[1] === 'e' && h.startsWith('H<span'), 'moved by ESC[H, on the character it is over', h);
  t.feed('\x1b[?25l');
  check(!/c4-term-cursor/.test(t.toHtml()), 'hidden by ESC[?25l', t.toHtml());
  t.feed('\x1b[?25h\x1b[3;10H');
  h = t.toHtml();
  check(/\nA:\\&gt;\n {9}<span class="c4-term-cursor c4-term-cursor-pad"><\/span>$/.test(h), 'shown by ESC[?25h, past the last line too', JSON.stringify(h));
}

console.log(fail ? `${T}: FAILED` : `${T}: OK`);
process.exit(fail);

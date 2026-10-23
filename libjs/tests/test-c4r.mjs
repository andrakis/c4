// test-c4r.mjs - libjs/c4r.js, the Code Explorer's reader, against every
// image the build leaves in src/c4bb/images (and any given as arguments).
//
// Each must read to its last byte, decode as instructions to the end of its
// code, and agree with itself: every code patch on an operand word, every
// function symbol, constructor and code-pointer target on an instruction.
// For the 32-bit ones, the code and data are loader.js's, which runs them.
//
//   node libjs/tests/test-c4r.mjs [files...]

import { readFileSync, readdirSync, statSync } from 'fs';
import { join } from 'path';
import { readC4r, analyse, insnText, PATCH } from '../c4r.js';
import { parseC4r } from '../../src/c4bb/sim/loader.js';

const root = new URL('../..', import.meta.url).pathname;
let files = process.argv.slice(2);
if (!files.length) {
  const walk = d => readdirSync(d).flatMap(n => {
    const p = join(d, n);
    return statSync(p).isDirectory() ? walk(p) : /\.c4[rol]$/.test(n) ? [p] : [];
  });
  files = walk(join(root, 'src/c4bb/images'));
}

let fail = 0, n = 0, words = 0, made = 0;
const bad = (f, why) => { fail++; console.log(`FAIL ${f}: ${why}`); };
for (const f of files) {
  let img, a;
  try { img = readC4r(readFileSync(f)); a = analyse(img); }
  catch (e) { bad(f, e.message); continue; }
  n++; words += img.code.length;
  if (img.trailing) bad(f, `${img.trailing} bytes after the last segment`);
  const first = a.insns.findIndex(i => !i.filler);
  const stray = a.insns.slice(first < 0 ? a.insns.length : first).filter(i => i.filler);
  if (stray.length) bad(f, `${stray.length} words that do not decode, first at ${stray[0].off}`);
  const boundary = o => o >= 0 && o < img.code.length && a.at[o] >= 0;
  let errs = 0;
  for (const pt of img.patches) {
    const inCode = !(pt.type === PATCH.DCODE || pt.type === PATCH.DDATA || pt.type === PATCH.DSYM);
    if (inCode && (a.at[pt.addr] !== -1 || !boundary(pt.addr - 1) || !('arg' in a.insns[a.at[pt.addr - 1]])))
      { if (errs++ < 3) bad(f, `patch at code word ${pt.addr} is not an operand`); }
    if ((pt.type === PATCH.CODE || pt.type === PATCH.DCODE) && !boundary(pt.val))
      { if (errs++ < 3) bad(f, `patch at ${pt.addr} points into the middle of an instruction (${pt.val})`); }
  }
  for (const fn of a.funcs) if (!boundary(fn.start)) bad(f, `${fn.name} starts mid-instruction (${fn.start})`);
  for (const c of [...img.cons, ...img.des]) if (!boundary(c)) bad(f, `constructor/destructor ${c} mid-instruction`);
  made += a.funcs.filter(x => x.made).length;
  if (img.wordBits === 32) {
    const l = parseC4r(readFileSync(f));
    if (l.code.length !== img.code.length || l.code.some((v, i) => v !== img.code[i])) bad(f, 'code differs from loader.js');
    if (l.data.length !== img.data.length || l.memsz !== img.memsz) bad(f, 'data differs from loader.js');
  }
}

// One pinned listing: hello32's main, as the explorer prints it
const h = analyse(readC4r(readFileSync(join(root, 'src/c4bb/images/hello32.c4r'))));
const m = h.funcs.find(x => x.name === 'main');
const text = h.insns.filter(i => i.off >= m.start && i.off < m.end).map(i => insnText(h, i, m)).join('; ');
console.log(`hello32 main: ${text}`);
if (!text.includes('IMM  "yello\\n"; PSH; PRTF')) bad('hello32.c4r', 'main does not print its string');

// The extension trailer and SMAP (docs/c4-explorer.md 2.1-2.2), which no
// compiler writes yet: one built by hand onto hello32, read back
{
  const img = readFileSync(join(root, 'src/c4bb/images/hello32.c4r'));
  const path = [...Buffer.from('src/tests/hello.c')];
  const payload = Buffer.from([1, 0, 1, path.length, ...path, 100,   // version, flags, a file of 100 bytes
    1, 1, 29, 4, ...Buffer.from('main'), 0, 3,                           // main: from word 1, 29 words, file 0 line 3
    2, 1 << 1 | 1, 0, 3, 10 << 1, 2]);                                   // rows: word 1 file 0 line 3; word 11 line 5
  const word = v => { const b = Buffer.alloc(4); b.writeInt32LE(v); return b; };
  const pad = Buffer.alloc((4 - payload.length % 4) % 4);
  const withMap = Buffer.concat([img, word(0x58), Buffer.from('SMAP'), word(payload.length), payload, pad, word(0)]);
  const m = readC4r(withMap);
  const r = m.smap && m.smap.rows.map(x => `${x.off}:${x.file}:${x.line}`).join(' ');
  if (m.trailing || !m.smap || m.smap.files[0].path !== 'src/tests/hello.c' || m.smap.funcs[0].name !== 'main' ||
      m.smap.funcs[0].start !== 1 || r !== '1:0:3 11:0:5')
    bad('hello32.c4r + SMAP', `read back as ${JSON.stringify(m.smap)} (${m.trailing} bytes left)`);
  else console.log(`hello32 with a map: rows ${r}`);
}

console.log(`c4r: ${n} files, ${words} code words, ${made} bodies without a symbol: ${fail ? fail + ' FAILED' : 'ok'}`);
process.exit(fail ? 1 : 0);

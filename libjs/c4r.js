// c4r.js - read a C4R image, object or library, and disassemble it.
//
// The Code Explorer's reader (docs/c4-explorer.md, E4), and anything else
// in JavaScript that wants an image's symbols as well as its code. It
// reads every segment, at either word size, where src/c4bb/sim/loader.js
// reads only what running needs and only at 32 bits.
//
// The opcode table is the machine's own: OPNAMES (the ROM, fused opcodes
// included) and c4m.js's hasOperand, so the disassembly is the encoding
// the machine runs and cannot drift from it.

import { OPNAMES } from '../src/c4bb/sim/devices.js';
import { OP, hasOperand } from './c4m.js';

// Patch types (load-c4r.c:80, c4rlink.c:89). A positive type is a code
// word that refers to that symbol id, unresolved.
export const PATCH = { CODE: -1, DATA: -2, DCODE: -3, DDATA: -4, DSYM: -5 };
// Symbol classes (load-c4r.c:103): functions' values are code words,
// globals' are data bytes.
export const SCLASS = { Num: 128, Fun: 129, Sys: 130, Glo: 131, Loc: 132, Id: 133 };
// Symbol attributes (c4rlink.c): an EXTERN record names a symbol another
// unit defines, and its value means nothing.
export const ATTR = { STATIC: 8, EXTERN: 16 };
const defined = s => !(s.attrs & ATTR.EXTERN);

export function opName(op) {
  if (op >= 0 && op * 5 + 4 <= OPNAMES.length) return OPNAMES.slice(op * 5, op * 5 + 4).trim();
  return `?${op}`;
}

// The image as its bytes say, segment by segment. Throws on anything that
// is not C4R, saying where.
export function readC4r(bytes) {
  if (bytes.length < 13 || bytes[0] !== 0x43 || bytes[1] !== 0x34 || bytes[2] !== 0x52)
    throw new Error('not a C4R file (no "C4R" magic)');
  const version = bytes[3], wordBits = bytes[4];
  if (wordBits !== 32 && wordBits !== 64) throw new Error(`a ${wordBits}-bit image: only 32 and 64 are read`);
  const W = wordBits / 8;
  const dv = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  let p = 5;
  const need = (n, what) => { if (p + n > bytes.length) throw new Error(`truncated in ${what} (at byte ${p})`); };
  const w = (what = 'header') => {
    need(W, what);
    const v = W === 4 ? dv.getInt32(p, true) : Number(dv.getBigInt64(p, true));
    p += W; return v;
  };
  const marker = (ch, what) => {
    need(W, what);
    if (bytes[p] !== ch.charCodeAt(0)) throw new Error(`expected the ${what} marker '${ch}' at byte ${p}`);
    p += W;
  };
  // Bytes 5-12 are MEMSZ: a word at 64 bits; at 32, a word and filler
  const memszWord = w(); if (W === 4) w();
  const entry = w(), codeLen = w(), dataLen = w(), patchLen = w(),
        symbolsLen = w(), consLen = w(), desLen = w();
  // v3's MEMSZ: the data segment's size in memory, BSS included
  const memsz = Math.max(dataLen, version >= 3 ? memszWord : dataLen);

  marker('C', 'code');
  const code = new Int32Array(codeLen);  // c4 code words fit 32 bits at either size
  for (let i = 0; i < codeLen; i++) code[i] = w('code');
  marker('D', 'data');
  need(dataLen, 'data');
  const data = bytes.slice(p, p + dataLen); p += dataLen;
  marker('P', 'patches');
  const patches = [];
  for (let i = 0; i < patchLen; i++) patches.push({ type: w('patches'), addr: w('patches'), val: w('patches') });
  marker('c', 'constructors');
  const cons = []; for (let i = 0; i < consLen; i++) cons.push(w('constructors'));
  marker('d', 'destructors');
  const des = []; for (let i = 0; i < desLen; i++) des.push(w('destructors'));

  const symbols = [];
  if (p < bytes.length) {
    marker('S', 'symbols');
    for (let i = 0; i < symbolsLen; i++) {
      const id = w('symbols'), type = w('symbols'), cls = w('symbols'), attrs = w('symbols');
      need(1, 'symbols'); const n = bytes[p++];
      need(n, 'symbols');
      const name = new TextDecoder('latin1').decode(bytes.subarray(p, p + n)); p += n;
      symbols.push({ id, type, cls, attrs, name, value: w('symbols') });
    }
  }

  // The extension trailer (docs/c4-explorer.md 2.1): 'X', then chunks of
  // tag, length and payload, padded to a word, until a 0 tag.
  const chunks = [];
  if (p + W <= bytes.length && bytes[p] === 0x58) {
    p += W;
    for (;;) {
      const tag = w('extension chunks');
      if (tag === 0) break;
      const len = w('extension chunks');
      need(len, 'extension chunk');
      const name = String.fromCharCode(tag & 255, (tag >> 8) & 255, (tag >> 16) & 255, (tag >>> 24) & 255);
      chunks.push({ tag: name, payload: bytes.subarray(p, p + len) });
      p += (len + W - 1) & ~(W - 1);
    }
  }
  const sm = chunks.find(c => c.tag === 'SMAP');
  const smap = sm ? readSmap(sm.payload) : null;

  return { version, wordBits, entry, code, data, memsz, patches, cons, des, symbols, chunks, smap,
           trailing: bytes.length - p, size: bytes.length };
}

// SMAP version 1 (docs/c4-explorer.md 2.2): files, functions and rows, in
// LEB128. No compiler writes it yet (track S); this is its reader.
export function readSmap(b) {
  let p = 0;
  const u = () => { let v = 0, s = 0, c; do { c = b[p++]; v += (c & 127) * 2 ** s; s += 7; } while (c & 128); return v; };
  const s = () => { let v = 0, sh = 0, c; do { c = b[p++]; v += (c & 127) * 2 ** sh; sh += 7; } while (c & 128);
                    return (c & 64) ? v - 2 ** sh : v; };
  const str = n => { const t = new TextDecoder('latin1').decode(b.subarray(p, p + n)); p += n; return t; };
  const version = b[p++], flags = b[p++];
  if (version !== 1) return { version, unknown: true };
  const files = [];
  for (let i = u(); i > 0; i--) {
    const path = str(u()), size = u();
    let crc = null;
    if (flags & 1) { crc = (b[p] | b[p + 1] << 8 | b[p + 2] << 16 | b[p + 3] << 24) >>> 0; p += 4; }
    files.push({ path, size, crc });
  }
  const funcs = [];
  let start = 0;
  for (let i = u(); i > 0; i--) {
    start += u();
    const len = u(), name = str(u()), file = u(), line = u();
    funcs.push({ start, len, name, file, line });
  }
  const rows = [];
  let off = 0, file = 0, line = 0;
  for (let i = u(); i > 0; i--) {
    const x = u();
    off += Math.floor(x / 2);
    if (x & 1) file = u();
    line += s();
    rows.push({ off, file, line });
  }
  return { version, flags, files, funcs, rows };
}

// Opcodes whose operand is relative to bp, in words.
const BP_REL = new Set([OP.LEA, OP.LDL, OP.STL, OP.PSHL, OP.LEAP, OP.JSRS]);
const BRANCH = new Set([OP.JMP, OP.BZ, OP.BNZ]);

// Everything the explorer shows about an image: the functions (named, and
// any body with no symbol), the globals, the decoded code with each
// operand resolved, and cross-references both ways.
export function analyse(img) {
  const { code, patches, symbols } = img;
  const byId = new Map(symbols.map(s => [s.id, s]));

  // Code-resident patches by the word they patch
  const codePatch = new Map(), dataPatch = new Map();
  for (const pt of patches) {
    if (pt.type === PATCH.DCODE || pt.type === PATCH.DDATA || pt.type === PATCH.DSYM) dataPatch.set(pt.addr, pt);
    else codePatch.set(pt.addr, pt);
  }

  // Decode from every place code is known to start (functions, the entry,
  // constructors and destructors) up to the next. c4 code holds nothing
  // but instructions, so from a start every word is an opcode or the
  // operand before it. c4's emitter writes from word 1 (`*++e`), so word
  // 0 is filler unless something starts there, and word 1 starts code.
  const starts = new Set([img.entry, ...img.cons, ...img.des]);
  for (const s of symbols) if (s.cls === SCLASS.Fun && defined(s)) starts.add(s.value);
  if (img.smap) for (const m of img.smap.funcs) starts.add(m.start);
  if (!starts.has(0)) starts.add(1);
  const order = [...starts].filter(o => o >= 0 && o < code.length).sort((a, b) => a - b);
  const insns = [];
  const at = new Int32Array(code.length).fill(-1);   // word -> instruction index
  const word = i => { at[i] = insns.length; insns.push({ off: i, op: code[i], name: '.word', arg: code[i], filler: true }); };
  for (let i = 0; i < (order.length ? order[0] : code.length); i++) word(i);
  for (let k = 0; k < order.length; k++) {
    const end = k + 1 < order.length ? order[k + 1] : code.length;
    for (let i = order[k]; i < end;) {
      const op = code[i], ins = { off: i, op, name: opName(op) };
      if (hasOperand(op) && i + 1 >= end) { word(i); break; }   // an operand cut off: not code
      at[i] = insns.length;
      if (hasOperand(op)) {
        ins.arg = code[i + 1];
        ins.patch = codePatch.get(i + 1) || null;
        i += 2;
      } else i += 1;
      insns.push(ins);
    }
  }

  // Functions: the symbol table's, then a body for every ENT that starts
  // none (a helper the compiler emitted without a symbol).
  const funcs = [];
  const seen = new Map();
  for (const s of symbols) {
    if (s.cls !== SCLASS.Fun || !defined(s) || s.value < 0 || s.value >= code.length) continue;
    const f = seen.get(s.value);
    if (f) { f.aliases.push(s.name); continue; }
    const fn = { name: s.name, start: s.value, aliases: [], sym: s, made: false };
    seen.set(s.value, fn); funcs.push(fn);
  }
  if (img.smap) for (const m of img.smap.funcs)
    if (!seen.has(m.start)) { const fn = { name: m.name, start: m.start, aliases: [], made: true }; seen.set(m.start, fn); funcs.push(fn); }
  for (const ins of insns)
    if (ins.op === OP.ENT && !ins.filler && !seen.has(ins.off)) {
      const fn = { name: `<code@${hex(ins.off)}>`, start: ins.off, aliases: [], made: true };
      seen.set(ins.off, fn); funcs.push(fn);
    }
  // the entry, constructors and word 1 when no symbol names them
  for (const o of order)
    if (!seen.has(o)) {
      const fn = { name: o === img.entry ? '<entry>' : `<code@${hex(o)}>`, start: o, aliases: [], made: true };
      seen.set(o, fn); funcs.push(fn);
    }
  if (!seen.has(0) && code.length) funcs.push({ name: '<filler>', start: 0, aliases: [], made: true });
  funcs.sort((a, b) => a.start - b.start);
  funcs.forEach((f, i) => {
    f.index = i;
    f.end = i + 1 < funcs.length ? funcs[i + 1].start : code.length;
    f.words = f.end - f.start;
    f.first = at[f.start] >= 0 ? at[f.start] : -1;
    f.callers = new Set(); f.callees = new Set(); f.globals = new Set();
    f.dups = 0;
  });
  // Static functions share names across units: say which is which
  const byName = new Map();
  for (const f of funcs) { const l = byName.get(f.name) || []; l.push(f); byName.set(f.name, l); }
  for (const l of byName.values()) if (l.length > 1) l.forEach((f, k) => { f.dups = l.length; f.nth = k + 1; });

  const funcAt = new Int32Array(code.length);
  for (const f of funcs) funcAt.fill(f.index, f.start, f.end);
  const funcOf = off => (off >= 0 && off < code.length) ? funcs[funcAt[off]] : null;

  // Globals, by data offset (BSS ones beyond the data bytes included)
  const globals = symbols.filter(s => s.cls === SCLASS.Glo && defined(s))
    .map(s => ({ name: s.name, off: s.value, sym: s, refs: new Set() }))
    .sort((a, b) => a.off - b.off);
  globals.forEach((g, i) => {
    g.index = i;
    const next = globals.slice(i + 1).find(h => h.off > g.off);
    g.size = (next ? next.off : img.memsz) - g.off;
    g.bss = g.off >= img.data.length;
  });
  const globalAt = off => {
    let lo = 0, hi = globals.length - 1, r = null;
    while (lo <= hi) { const m = (lo + hi) >> 1; if (globals[m].off <= off) { r = globals[m]; lo = m + 1; } else hi = m - 1; }
    return r && off < r.off + Math.max(r.size, 1) ? r : null;
  };

  // Resolve each operand to what it names
  for (const ins of insns) {
    const f = funcOf(ins.off);
    const pt = ins.patch;
    if (!('arg' in ins) || ins.filler) continue;
    if (pt && pt.type === PATCH.CODE) {
      const t = funcOf(pt.val);
      if (BRANCH.has(ins.op) && t === f) ins.ref = { kind: 'label', off: pt.val };
      else if (t && t.start === pt.val) ins.ref = { kind: 'func', func: t, off: pt.val };
      else ins.ref = { kind: 'code', func: t, off: pt.val };
      if (t && t !== f && (ins.op === OP.JSR)) { f.callees.add(t); t.callers.add(f); }
      else if (t && t !== f) { f.callees.add(t); t.callers.add(f); ins.ref.taken = true; }
    } else if (pt && pt.type === PATCH.DATA) {
      let g = globalAt(pt.val);
      const s = cString(img.data, pt.val);
      // A unit's string literals follow its globals and have no symbol,
      // so "up to the next symbol" takes them into the last global. One
      // that starts a string past that global's first word is a literal.
      if (g && s !== null && s.length >= 2 && pt.val - g.off >= img.wordBits / 8 && img.data[pt.val - 1] === 0) g = null;
      ins.ref = { kind: 'data', off: pt.val, global: g, delta: g ? pt.val - g.off : 0 };
      if (g) { g.refs.add(f); f.globals.add(g); }
      else if (s !== null) ins.ref.str = s;
    } else if (pt && pt.type > 0) {
      const s = byId.get(pt.type);
      ins.ref = { kind: 'extern', name: s ? s.name : `symbol ${pt.type}`, sym: s };
    } else if (BP_REL.has(ins.op)) {
      ins.ref = { kind: 'bp', n: ins.arg };
    }
  }

  // Branch targets inside each function get labels, in order
  for (const f of funcs) {
    const targets = new Set();
    for (let k = f.first; k >= 0 && k < insns.length && insns[k].off < f.end; k++) {
      const r = insns[k].ref;
      if (r && r.kind === 'label') targets.add(r.off);
    }
    f.labels = new Map([...targets].sort((a, b) => a - b).map((o, j) => [o, `L${j + 1}`]));
  }

  // Data-resident pointers: what an initialized global points at
  const dataRefs = [];
  for (const pt of dataPatch.values()) {
    const g = globalAt(pt.addr);
    let target;
    if (pt.type === PATCH.DCODE) target = { kind: 'func', func: funcOf(pt.val), off: pt.val };
    else if (pt.type === PATCH.DDATA) target = { kind: 'data', off: pt.val, global: globalAt(pt.val) };
    else { const s = byId.get(pt.val); target = { kind: 'extern', name: s ? s.name : `symbol ${pt.val}` }; }
    if (target.kind === 'func' && target.func && g) g.refs.add(target.func);
    dataRefs.push({ addr: pt.addr, global: g, target });
  }

  return { img, insns, at, funcs, funcOf, globals, globalAt, byName, dataPatch, dataRefs };
}

// The NUL-terminated printable string at a data offset, or null
export function cString(data, off, max = 200) {
  if (off < 0 || off >= data.length) return null;
  let s = '';
  for (let i = off; i < data.length; i++) {
    const c = data[i];
    if (c === 0) return s;
    // tabs, newlines and ESC are kept as they are; JSON.stringify shows them
    if ((c < 32 && c !== 9 && c !== 10 && c !== 13 && c !== 27) || c > 126) return null;
    s += String.fromCharCode(c);
    if (s.length >= max) return s + '...';
  }
  // the data's end: c4lc leaves the last literal's NUL to the zeroed
  // memory after it (an object's data can be exactly "%d\n")
  return s;
}

export function hex(n) { return '0x' + (n >>> 0).toString(16); }

// One instruction as text, for tools and tests: "IMM  &main", "JSR  printf"
export function insnText(a, ins, fn) {
  if (!('arg' in ins)) return ins.name;
  if (ins.filler) return `.word ${ins.arg}`;
  const r = ins.ref;
  let t;
  if (!r) t = String(ins.arg);
  else if (r.kind === 'label') t = (fn && fn.labels.get(r.off)) || hex(r.off);
  else if (r.kind === 'func') t = (ins.op === OP.JSR ? '' : '&') + r.func.name;
  else if (r.kind === 'code') t = r.func ? `${r.func.name}+${r.off - r.func.start}` : hex(r.off);
  else if (r.kind === 'data') t = r.global ? (ins.op === OP.JSRI ? '*' : '&') + r.global.name + (r.delta ? `+${r.delta}` : '')
                                 : r.str !== undefined ? JSON.stringify(r.str) : `data+${r.off}`;
  else if (r.kind === 'extern') t = r.name;
  else if (r.kind === 'bp') t = r.n >= 0 ? `[bp+${r.n}]` : `[bp${r.n}]`;
  return `${ins.name.padEnd(5)}${t}`;
}

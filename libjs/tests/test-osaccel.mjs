// test-osaccel.mjs [FILE...] -- C4IX's accelerators (src/c4ix/accel.mjs,
// docs/accel.md 7) against zlib and against libarc's own C: the inflater of
// src/c4ix/lib/archive.c carried over line for line, handing its loop to
// CX_INFLATE where the C does, on a flat machine of its own. The input
// arrives in pieces of odd sizes (in_more) and leaves through small windows
// (out_flush), so the loop meets every edge; each stream is inflated with
// the accelerator and without, and must give the same bytes, use the same
// input, and on a damaged stream fail with the same message. CRC-32 is held
// to zlib's, whole and in two. FILEs are more inputs (default: archive.c and
// the desktop's image). test-libjs runs it. Prints "N cases, F failed".
import { readFileSync, existsSync } from 'node:fs';
import { gzipSync, deflateRawSync, constants } from 'node:zlib';
import { kit } from '../accel.js';
import c4ixAccel from '../../src/c4ix/accel.mjs';
const ops = c4ixAccel(kit);
const [CRC32, INFLATE] = ops;

const MEM = 64 << 20;
const u8 = new Uint8Array(MEM), i32 = new Int32Array(u8.buffer);
const m = { u8, i32, vm: false, uwin: false, sp: 0, a: 0, guardLo: () => MEM,
  ld8: x => u8[x], st8: (x, b) => { u8[x] = b; }, ld32: x => i32[x >> 2], st32: (x, v) => { i32[x >> 2] = v; } };
let brk = 0x10000;
const alloc = n => { const p = brk; brk = (brk + n + 15) & ~15; return p; };
const call = (op, ...a) => { const sp = 0x8000; for (let i = 0; i < a.length; i++) i32[(sp + 4 * (a.length - i)) >> 2] = a[i]; m.sp = sp; return op(m) | 0; };

// ---- archive.c, ported ----
const MAXBITS = 15, FASTBITS = 9, FASTSIZE = 512, WINDOW = 32768;
const lbase = [3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258];
const lext = [0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0];
const dbase = [1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577];
const dext = [0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13];
const clorder = [16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15];
const huff = nsym => ({ count: alloc(4 * (MAXBITS + 1)), symbol: alloc(4 * nsym), fast: alloc(4 * FASTSIZE) });
const W = (a, i, v) => { i32[(a >> 2) + i] = v; }, R = (a, i) => i32[(a >> 2) + i];

let src, srcPos, in_buf, in_cap, in_len, in_pos, out_buf, out_cap, out_pos, bitbuf, bitcnt, in_short, out, err, accel, nAccel, st;
const fail = why => { err = why; return -1; };
function in_more() {
  let start = in_pos - 4; if (start < 0) start = 0;
  if (start > 0) { u8.copyWithin(in_buf, in_buf + start, in_buf + in_len); in_pos -= start; in_len -= start; }
  if (in_len >= in_cap) return 0;
  const n = Math.min(in_cap - in_len, src.length - srcPos, 1 + ((srcPos * 7919) % in_cap));
  if (n <= 0) return 0;
  u8.set(src.subarray(srcPos, srcPos + n), in_buf + in_len); srcPos += n; in_len += n; return 1;
}
const in_need = n => { while (in_len - in_pos < n) if (!in_more()) return 0; return 1; };
function out_flush() {
  const n = out_pos - WINDOW;
  out.push(u8.slice(out_buf, out_buf + n));
  u8.copyWithin(out_buf, out_buf + n, out_buf + n + WINDOW); out_pos = WINDOW; return 0;
}
function bits(need) {
  while (bitcnt < need) { if (in_pos >= in_len && !in_more()) { in_short = 1; return 0; } bitbuf |= u8[in_buf + in_pos++] << bitcnt; bitcnt += 8; }
  const v = bitbuf & ((1 << need) - 1); bitbuf >>= need; bitcnt -= need; return v;
}
function construct(h, len, n) {
  for (let l = 0; l <= MAXBITS; l++) W(h.count, l, 0);
  for (let s = 0; s < n; s++) W(h.count, len[s], R(h.count, len[s]) + 1);
  for (let k = 0; k < FASTSIZE; k++) W(h.fast, k, 0);
  if (R(h.count, 0) === n) return 0;
  let left = 1;
  for (let l = 1; l <= MAXBITS; l++) { left <<= 1; left -= R(h.count, l); if (left < 0) return left; }
  const offs = [], next = []; offs[1] = 0;
  for (let l = 1; l < MAXBITS; l++) offs[l + 1] = offs[l] + R(h.count, l);
  for (let s = 0; s < n; s++) if (len[s]) { W(h.symbol, offs[len[s]], s); offs[len[s]]++; }
  let code = 0; next[1] = 0;
  for (let l = 1; l < MAXBITS; l++) { code = (code + R(h.count, l)) << 1; next[l + 1] = code; }
  for (let s = 0; s < n; s++) { const l = len[s]; if (!l) continue; code = next[l]; next[l] = code + 1;
    if (l <= FASTBITS) { let r = 0; for (let j = 0; j < l; j++) r = (r << 1) | ((code >> j) & 1); for (; r < FASTSIZE; r += 1 << l) W(h.fast, r, s * 16 + l); } }
  return left;
}
function decode(h) {
  while (bitcnt < FASTBITS && in_pos < in_len) { bitbuf |= u8[in_buf + in_pos++] << bitcnt; bitcnt += 8; }
  const e = R(h.fast, bitbuf & (FASTSIZE - 1));
  if (e && (e & 15) <= bitcnt) { const l = e & 15; bitbuf >>= l; bitcnt -= l; return e >> 4; }
  let code = 0, first = 0, index = 0;
  for (let l = 1; l <= MAXBITS; l++) { code |= bits(1); if (in_short) return -1; const c = R(h.count, l);
    if (code - c < first) return R(h.symbol, index + (code - first)); index += c; first += c; first <<= 1; code <<= 1; }
  return -10;
}
function codes(lc, dc) {
  const lfast = lc.fast, dfast = dc.fast, ib = in_buf, ob = out_buf, oc = out_cap;
  let il = in_len, bb = bitbuf, bc = bitcnt, ip = in_pos, op = out_pos, sym, len, dist, e, l;
  for (;;) {
    if (accel && op + 258 <= oc && il - ip >= 16) {
      const v = [bb, bc, ip, il, op, oc, ib, ob, lfast, dfast, lc.count, lc.symbol, dc.count, dc.symbol];
      v.forEach((x, k) => W(st, k, x));
      e = call(INFLATE, st); nAccel++;
      bb = R(st, 0); bc = R(st, 1); ip = R(st, 2); op = R(st, 4);
      if (e === 1) break;
    }
    if (bc < 16 && ip + 2 <= il) { bb = bb | (u8[ib + ip] << bc) | (u8[ib + ip + 1] << (bc + 8)); ip += 2; bc += 16; }
    e = R(lfast, bb & (FASTSIZE - 1));
    if (e && (l = e & 15) <= bc) { bb >>= l; bc -= l; sym = e >> 4; }
    else { bitbuf = bb; bitcnt = bc; in_pos = ip; sym = decode(lc); bb = bitbuf; bc = bitcnt; ip = in_pos; il = in_len; if (sym < 0) return fail('bad literal/length code'); }
    if (sym < 256) { if (op >= oc) { out_pos = op; if (out_flush() < 0) return -1; op = out_pos; } u8[ob + op++] = sym; continue; }
    if (sym === 256) break;
    sym -= 257; if (sym >= 29) return fail('bad length symbol');
    len = lbase[sym];
    if ((l = lext[sym])) { bitbuf = bb; bitcnt = bc; in_pos = ip; len += bits(l); bb = bitbuf; bc = bitcnt; ip = in_pos; il = in_len; }
    if (bc < 16 && ip + 2 <= il) { bb = bb | (u8[ib + ip] << bc) | (u8[ib + ip + 1] << (bc + 8)); ip += 2; bc += 16; }
    e = R(dfast, bb & (FASTSIZE - 1));
    if (e && (l = e & 15) <= bc) { bb >>= l; bc -= l; sym = e >> 4; }
    else { bitbuf = bb; bitcnt = bc; in_pos = ip; sym = decode(dc); bb = bitbuf; bc = bitcnt; ip = in_pos; il = in_len; }
    if (sym < 0 || sym >= 30) return fail('bad distance symbol');
    dist = dbase[sym];
    if ((l = dext[sym])) { bitbuf = bb; bitcnt = bc; in_pos = ip; dist += bits(l); bb = bitbuf; bc = bitcnt; ip = in_pos; il = in_len; }
    if (in_short) return fail('input ends mid-block');
    if (dist > op) return fail('distance reaches before the start');
    if (op + len > oc) { out_pos = op; if (out_flush() < 0) return -1; op = out_pos; }
    let from = ob + op - dist;
    for (; len > 0; len--) u8[ob + op++] = u8[from++];
  }
  bitbuf = bb; bitcnt = bc; in_pos = ip; out_pos = op; return 0;
}
let lencode, distcode, fixlen, fixdist;
function stored() {
  bitbuf = 0; in_pos -= bitcnt >> 3; bitcnt = 0;
  if (!in_need(4)) return fail('input ends in a stored header');
  const len = u8[in_buf + in_pos] | (u8[in_buf + in_pos + 1] << 8), nlen = u8[in_buf + in_pos + 2] | (u8[in_buf + in_pos + 3] << 8);
  in_pos += 4; if (len !== (0xFFFF ^ nlen)) return fail('stored block length check');
  if (!in_need(len)) return fail('input ends in a stored block');
  if (out_pos + len > out_cap && out_flush() < 0) return -1;
  u8.copyWithin(out_buf + out_pos, in_buf + in_pos, in_buf + in_pos + len); out_pos += len; in_pos += len; return 0;
}
function fixed() {
  if (!fixlen) { fixlen = huff(288); fixdist = huff(30); const L = [];
    let s = 0; while (s < 144) L[s++] = 8; while (s < 256) L[s++] = 9; while (s < 280) L[s++] = 7; while (s < 288) L[s++] = 8;
    construct(fixlen, L, 288); for (s = 0; s < 30; s++) L[s] = 5; construct(fixdist, L, 30); }
  return codes(fixlen, fixdist);
}
function dynamic() {
  const nlen = bits(5) + 257, ndist = bits(5) + 1, ncode = bits(4) + 4, L = [];
  if (in_short) return fail('input ends in a block header');
  if (nlen > 286 || ndist > 30) return fail('bad code counts');
  let index = 0; for (; index < ncode; index++) L[clorder[index]] = bits(3); for (; index < 19; index++) L[clorder[index]] = 0;
  if (construct(lencode, L, 19) !== 0) return fail('bad code-length code');
  index = 0;
  while (index < nlen + ndist) {
    const sym = decode(lencode); if (sym < 0) return fail('bad code-length symbol');
    if (sym < 16) L[index++] = sym;
    else { let len = 0, rep; if (sym === 16) { if (!index) return fail('repeat'); len = L[index - 1]; rep = 3 + bits(2); }
      else if (sym === 17) rep = 3 + bits(3); else rep = 11 + bits(7);
      if (index + rep > nlen + ndist) return fail('too many lengths'); while (rep-- > 0) L[index++] = len; }
  }
  if (!L[256]) return fail('no end-of-block code');
  let e = construct(lencode, L, nlen);
  if (e < 0 || (e > 0 && nlen - R(lencode.count, 0) !== 1)) return fail('bad literal/length lengths');
  e = construct(distcode, L.slice(nlen), ndist);
  if (e < 0 || (e > 0 && ndist - R(distcode.count, 0) !== 1)) return fail('bad distance lengths');
  return codes(lencode, distcode);
}
function inflateStream(raw, incap, wincap, useAccel) {
  brk = 0x10000; fixlen = null; lencode = huff(286); distcode = huff(30); st = alloc(56);
  src = raw; srcPos = 0; in_cap = incap; in_buf = alloc(incap); in_len = 0; in_pos = 0;
  out_cap = wincap; out_buf = alloc(wincap); out_pos = 0; bitbuf = 0; bitcnt = 0; in_short = 0; out = []; err = null;
  accel = useAccel; nAccel = 0;
  in_more();
  let last = 0;
  while (!last) {
    last = bits(1); const type = bits(2);
    if (in_short) { fail('input ends before a block'); break; }
    const r = type === 0 ? stored() : type === 1 ? fixed() : type === 2 ? dynamic() : fail('bad block type');
    if (r < 0) break;
  }
  if (!err) out.push(u8.slice(out_buf, out_buf + out_pos));
  return { err, data: Buffer.concat(out), used: srcPos - (in_len - in_pos) - (bitcnt >> 3), n: nAccel };
}

// ---- the cases ----
let fails = 0, cases = 0;
const check = (ok, what) => { cases++; if (!ok) { fails++; console.log('FAIL', what); } };
const inputs = [];
const ROOT = new URL('../../', import.meta.url);
const files = process.argv.slice(2).length ? process.argv.slice(2)
  : ['src/c4ix/lib/archive.c', 'src/c4bb/images/disk/c4ix-desktop.c4r'].map(f => new URL(f, ROOT)).filter(f => existsSync(f));
for (const f of files) inputs.push([String(f).replace(/^.*\/(src\/)/, '$1'), readFileSync(f)]);
// made-up data: text-ish, runs, random, short overlapping matches
let seed = 12345; const rnd = () => (seed = (Math.imul(seed, 1103515245) + 12345) >>> 0) / 2 ** 32;
const mk = (n, kind) => { const b = Buffer.alloc(n); for (let i = 0; i < n; i++) b[i] =
  kind === 0 ? 'the quick brown fox jumps over the lazy dog\n'.charCodeAt((i * 7 + (i >> 9)) % 44) :
  kind === 1 ? (rnd() < 0.01 ? (rnd() * 256) | 0 : (i >> 6) & 255) : kind === 2 ? (rnd() * 256) | 0 : (i % 3 ? 'a' : 'b').charCodeAt(0); return b; };
for (const [n, k] of [[0, 0], [1, 2], [300, 3], [5000, 0], [70000, 1], [200000, 2], [400000, 0], [1 << 20, 1]]) inputs.push([`made ${k} ${n}`, mk(n, k)]);
for (const [name, data] of inputs) {
  for (const level of [1, 6, 9]) for (const strategy of [constants.Z_DEFAULT_STRATEGY, constants.Z_FIXED, constants.Z_HUFFMAN_ONLY, constants.Z_RLE]) {
    const raw = deflateRawSync(data, { level, strategy });
    for (const [incap, wincap] of [[131072, 262144], [4096, WINDOW + 300], [700, WINDOW + 1000], [131072, Math.max(data.length, 300) + 300]]) {
      if (wincap < data.length && wincap < WINDOW + 258) continue;
      const a = inflateStream(raw, incap, wincap, false), b = inflateStream(raw, incap, wincap, true);
      // a stored block bigger than a small input buffer is an error for the C too: the same one
      if (a.err) { check(incap < 66000 && b.err === a.err, `${name} C l${level} s${strategy} ${incap}/${wincap}: ${a.err} / ${b.err}`); continue; }
      check(a.data.equals(data), `${name} C l${level} s${strategy} ${incap}/${wincap}`);
      check(!b.err && b.data.equals(data) && b.used === a.used, `${name} accel l${level} s${strategy} ${incap}/${wincap}: ${b.err} used ${b.used}/${a.used}`);
    }
  }
  // damaged: flip bits; both must agree (the same error, or the same bytes)
  const raw = deflateRawSync(data);
  for (let t = 0; t < 40 && raw.length > 2; t++) {
    const bad = Buffer.from(raw); const at = (rnd() * bad.length) | 0; bad[at] ^= 1 << ((rnd() * 8) | 0);
    const a = inflateStream(bad, 4096, WINDOW + 300, false), b = inflateStream(bad, 4096, WINDOW + 300, true);
    check(a.err === b.err && a.data.equals(b.data), `${name} damaged at ${at}: C ${a.err} accel ${b.err}`);
  }
  // CRC
  brk = 0x10000; const p = alloc(data.length + 1); u8.set(data, p);
  const want = (gzipSync(data).readUInt32LE(gzipSync(data).length - 8)) | 0;
  check(call(CRC32, 0, p, data.length) === want, `${name} crc`);
  if (data.length > 10) { const h = call(CRC32, 0, p, 7); check(call(CRC32, h, p + 7, data.length - 7) === want, `${name} crc in two`); }
}
console.log(`${cases} cases, ${fails} failed`);
process.exit(fails ? 1 : 0);

// lh5.js - one -lh5- member of an LHA archive, decoded: what
// games/quake/tools/unlh5.c does, in JS, for unpack.js (id's shareware
// Quake is an LHA archive, resource.1, in a zip).
//
// unlh5(src, offset, packed, size): the SIZE bytes the PACKED bytes of SRC
// at OFFSET decode to. The stream is LHA's -lh5-: blocks, each a count of
// codes and three Huffman tables (the code lengths' own, the literals and
// lengths', the distances'), the window 8 KB. A table is canonical: by
// length, then by symbol, as unlh5.c's build() makes it. Here each is
// looked up whole, 1 << (its longest code) entries, rather than a bit at a
// time: 18.7 MB in a few hundred milliseconds. A stream that is not lh5
// throws; that it decoded right is for the caller to check (unpack.js
// checks its SHA-256).

// A canonical Huffman table of LEN[0..N): { single, max, table, len }.
// table[the next MAX bits] is the symbol, and len[symbol] the bits it
// takes. single >= 0: a table of one symbol, which takes no bits.
function table(len, n) {
  let max = 0;
  for (let i = 0; i < n; i++) if (len[i] > max) max = len[i];
  if (!max) throw new Error('lh5: an empty table');
  const size = 1 << max, t = new Uint16Array(size).fill(0xffff);
  let code = 0;
  for (let l = 1; l <= max; l++) {
    for (let i = 0; i < n; i++) {
      if (len[i] !== l) continue;
      const shift = max - l, at = code << shift, to = (code + 1) << shift;
      if (to > size) throw new Error('lh5: a table with too many codes');
      t.fill(i, at, to);
      code++;
    }
    code <<= 1;
  }
  return { single: -1, max, table: t, len: len.slice(0, n) };
}
const single = s => ({ single: s, max: 0, table: null, len: null });

export function unlh5(src, offset, packed, size) {
  const out = new Uint8Array(size);
  const end = Math.min(src.length, offset + packed);
  let ip = offset;
  // the bits not yet taken, the first of them highest: BC of them in BB.
  // Past the end they are 0, as unlh5.c's are.
  let bb = 0, bc = 0;
  const bits = n => {
    while (bc < n) { bb = (bb << 8) | (ip < end ? src[ip++] : 0); bc += 8; }
    bc -= n;
    const v = bb >>> bc;
    bb &= (1 << bc) - 1;
    return v;
  };
  const decode = h => {
    if (h.single >= 0) return h.single;
    while (bc < h.max) { bb = (bb << 8) | (ip < end ? src[ip++] : 0); bc += 8; }
    const s = h.table[bb >>> (bc - h.max)];
    if (s === 0xffff) throw new Error(`lh5: no such code, at ${ip}`);
    bc -= h.len[s];
    bb &= (1 << bc) - 1;
    return s;
  };
  // the code lengths' table, and the distances': NN symbols, NBIT bits
  // for how many; after the SPECIAL'th length, a 2-bit run of zeros
  const readPt = (nn, nbit, special) => {
    const len = new Uint8Array(40);
    const n = bits(nbit);
    if (!n) return single(bits(nbit));
    let i = 0;
    while (i < n && i < 32) {
      let c = bits(3);
      if (c === 7) while (bits(1)) c++;
      len[i++] = c;
      if (i === special) for (let z = bits(2); z--;) len[i++] = 0;
    }
    return table(len, nn);
  };
  // the literals' and lengths' table, its lengths coded by T
  const readC = t => {
    const len = new Uint8Array(512 + 532);
    const n = bits(9);
    if (!n) return single(bits(9));
    let i = 0;
    while (i < n) {
      const c = decode(t);
      if (c <= 2) for (let k = c === 0 ? 1 : c === 1 ? bits(4) + 3 : bits(9) + 20; k--;) len[i++] = 0;
      else len[i++] = c - 2;
    }
    return table(len, 510);
  };
  let op = 0;
  while (op < size) {
    if (ip >= end && bc < 16) throw new Error(`lh5: the stream ended at ${op} of ${size} bytes`);
    let blk = bits(16);
    const T = readPt(19, 5, 3), C = readC(T), P = readPt(14, 4, -1);
    while (blk-- > 0 && op < size) {
      const c = decode(C);
      if (c < 256) { out[op++] = c; continue; }
      let n = c - 256 + 3;
      const p = decode(P);
      const d = p ? (1 << (p - 1)) + (p > 1 ? bits(p - 1) : 0) : 0;
      let from = op - d - 1;
      if (n > size - op) n = size - op;
      // before the start of the output, the window is 0s
      for (; n > 0 && from < 0; n--, from++) out[op++] = 0;
      for (; n > 0; n--) out[op++] = out[from++];
    }
  }
  return out;
}

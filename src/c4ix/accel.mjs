// accel.mjs -- C4IX's accelerators for c4's libjs (libjs/accel.js,
// docs/accel.md 7): libarc's CRC-32 and its inflater's literal/match loop,
// which were 85% of the instructions a boot runs, unpacking the root archive.
// listed as "os/c4ix", so the machine's switch for an operating system's
// accelerators decides whether a program is given them (ACC_FIND).
// src/c4ix/include/c4ix_accel.h is the C side, with the operations' numbers
// (CX_*); src/c4ix/lib/archive.c calls them and keeps its own C for every
// other machine. Each does what that C does: the same bytes, the same state
// left behind; where its C would meet an edge (the window to flush, the input
// to read on, a bad code) it stops before that turn and the C takes it.
export default function c4ix({ reader, writer, reader32, writer32, probe, args }) {
  // CX_CRC32 (crc, buf, len): arc_crc32, the table's CRC-32, gives the CRC.
  // Reads only, so a fault part way through runs it again from the start.
  const T = new Int32Array(256);
  for (let n = 0; n < 256; n++) {
    let c = n;
    for (let k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320 ^ (c >>> 1) : c >>> 1;
    T[n] = c;
  }
  function crc32(m) {
    let [crc, buf, len] = args(m, 3);
    if (len <= 0) return crc;
    probe(m, buf, len, false);
    const rd = reader(m);
    crc = ~crc;
    for (let i = 0; i < len; i++) crc = T[(crc ^ rd((buf + i) | 0)) & 255] ^ (crc >>> 8);
    return ~crc;
  }

  // RFC 1951's lengths and distances, archive.c's lbase, lext, dbase, dext
  const LBASE = [3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
    35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258];
  const LEXT = [0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0];
  const DBASE = [1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
    257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577];
  const DEXT = [0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13];
  const MAXBITS = 15, FASTMASK = 511, FASTBITS = 9, WINDOW = 32768;
  const MATCHMAX = 258, INMARGIN = 16, CHUNK = 65536;

  // CX_INFLATE (st): codes(), from the state in st -- bitbuf, bitcnt,
  // in_pos, in_len, out_pos, out_cap, in_buf, out_buf, the literal/length and
  // distance codes' fast tables, then their count and symbol arrays -- turn
  // after turn while a whole turn can neither reach out_cap nor read past
  // in_len, at most CHUNK of each a call, and writes back the state after the
  // last whole turn. 1 at the end of the block (the state past its code), 0
  // where the C is to take the next turn. Everything it may touch is probed
  // first, so nothing faults part way and a run again from the start is the
  // same run.
  function inflate(m) {
    const [st] = args(m, 1);
    const r32 = reader32(m);
    let bb = r32(st), bc = r32(st + 4), ip = r32(st + 8);
    const il = r32(st + 12);
    let op = r32(st + 16);
    const oc = r32(st + 20), ib = r32(st + 24), ob = r32(st + 28);
    const lfast = r32(st + 32), dfast = r32(st + 36);
    const lcount = r32(st + 40), lsym = r32(st + 44), dcount = r32(st + 48), dsym = r32(st + 52);
    if (op + MATCHMAX > oc || il - ip < INMARGIN) return 0;
    // at most CHUNK of output and of input a time: what it probes stays
    // small (an arc_gunzip's window is the whole file), and the C asks again
    const oend = Math.min(oc, op + CHUNK), iend = Math.min(il, ip + CHUNK);
    probe(m, st, 56, true);
    probe(m, ib + ip, iend - ip, false);
    probe(m, ob + Math.max(0, op - WINDOW), oend - Math.max(0, op - WINDOW), true);
    probe(m, lfast, 4 * (FASTMASK + 1), false); probe(m, dfast, 4 * (FASTMASK + 1), false);
    probe(m, lcount, 4 * (MAXBITS + 1), false); probe(m, dcount, 4 * (MAXBITS + 1), false);
    probe(m, lsym, 4 * 288, false); probe(m, dsym, 4 * 30, false);
    const rin = reader(m), rout = reader(m), wout = writer(m);
    const tab = reader32(m), syms = reader32(m);
    const inb = i => rin((ib + i) | 0);
    let ok = true;      // a turn met an edge: stop before it

    // decode(): the slow path, a long code a bit at a time, through bits(1)
    function bits(need) {
      while (bc < need) {
        if (ip >= il) { ok = false; return 0; }
        bb |= inb(ip++) << bc;
        bc += 8;
      }
      const v = bb & ((1 << need) - 1);
      bb >>= need; bc -= need;
      return v;
    }
    function decode(fast, count, sym) {
      while (bc < FASTBITS && ip < il) { bb |= inb(ip++) << bc; bc += 8; }
      const e = tab(fast + 4 * (bb & FASTMASK));
      if (e && (e & 15) <= bc) { const l = e & 15; bb >>= l; bc -= l; return e >> 4; }
      let code = 0, first = 0, index = 0;
      for (let l = 1; l <= MAXBITS; l++) {
        code |= bits(1);
        if (!ok) return -1;
        const n = tab(count + 4 * l);
        if (code - n < first) return syms(sym + 4 * (index + (code - first)));
        index += n; first += n; first <<= 1; code <<= 1;
      }
      return -10;
    }

    let res = 0;
    for (;;) {
      if (op + MATCHMAX > oend || iend - ip < INMARGIN) break;
      const sbb = bb, sbc = bc, sip = ip;      // the turn's start, to stop at
      // a literal/length symbol
      if (bc < 16 && ip + 2 <= il) { bb = bb | (inb(ip) << bc) | (inb(ip + 1) << (bc + 8)); ip += 2; bc += 16; }
      let e = tab(lfast + 4 * (bb & FASTMASK)), sym, l;
      if (e && (l = e & 15) <= bc) { bb >>= l; bc -= l; sym = e >> 4; }
      else sym = decode(lfast, lcount, lsym);
      if (!ok || sym < 0) { bb = sbb; bc = sbc; ip = sip; break; }
      if (sym < 256) { wout(ob + op, sym); op++; continue; }
      if (sym === 256) { res = 1; break; }
      sym -= 257;
      if (sym >= 29) { bb = sbb; bc = sbc; ip = sip; break; }
      let len = LBASE[sym];
      if ((l = LEXT[sym])) len += bits(l);
      // its distance
      if (bc < 16 && ip + 2 <= il) { bb = bb | (inb(ip) << bc) | (inb(ip + 1) << (bc + 8)); ip += 2; bc += 16; }
      e = tab(dfast + 4 * (bb & FASTMASK));
      if (e && (l = e & 15) <= bc) { bb >>= l; bc -= l; sym = e >> 4; }
      else sym = decode(dfast, dcount, dsym);
      if (!ok || sym < 0 || sym >= 30) { bb = sbb; bc = sbc; ip = sip; break; }
      let dist = DBASE[sym];
      if ((l = DEXT[sym])) dist += bits(l);
      if (!ok || dist > op) { bb = sbb; bc = sbc; ip = sip; break; }
      // a copy that does not overlap is memcpy's; one that does, byte by
      // byte forward: the same bytes either way
      let from = ob + op - dist, to = ob + op;
      op += len;
      for (; len > 0; len--) wout(to++, rout(from++));
    }
    const w32 = writer32(m);
    w32(st, bb); w32(st + 4, bc); w32(st + 8, ip); w32(st + 16, op);
    return res;
  }

  return [crc32, inflate];
}

// fpu.js -- the FPU opcode (89, F3; docs/c4-float.md) for the JS machines:
// libjs (libjs/c4m.js) and c4bb's simulator (machine.js).
//
// On the stack: sp[0] the sub-op, sp[1] a, sp[2] b, sp[3] r. A float is
// its bits in a word; a double is the address of its two words, the low
// one first, and a double result is written where r points. The answer to
// every sub-op is src/c4fp/softfp.c's to the bit -- the guest's own
// helpers, which use this only when the info word says C4I_FPU -- so:
//   - the arithmetic is the host's IEEE doubles, which are exact for
//     + - * / and sqrt, and for floats too when the double result is
//     rounded to single (Math.fround): 53 bits are more than twice 24 + 2;
//   - NaNs follow x86 SSE, as softfp.c does, not JavaScript (which keeps
//     one NaN): an operation on a NaN gives its first NaN operand,
//     quieted; an invalid one (inf - inf, 0 * inf, 0 / 0, sqrt of a
//     negative) gives the default NaN, negative and quiet;
//   - a conversion to int that does not fit gives the most negative int.
//
//   1 add  2 sub  3 mul  4 div  5 sqrt  6 cmp  7 from int  8 to int       (float)
//   17 add 18 sub 19 mul 20 div 21 sqrt 22 cmp 23 from int 24 to int      (double)
//   25 float to double   26 double to float

export const C4I_FPU = 0x10000;

const F = new Float32Array(1), FU = new Uint32Array(F.buffer);
const D = new Float64Array(1), DU = new Uint32Array(D.buffer);   // DU[0] low word, DU[1] high
const LITTLE = new Uint8Array(new Uint32Array([1]).buffer)[0] === 1;
const LO = LITTLE ? 0 : 1, HI = LITTLE ? 1 : 0;

const fval = u => { FU[0] = u; return F[0]; };
const fbits = x => { F[0] = x; return FU[0] | 0; };
const fnan = u => (u & 0x7fffffff) > 0x7f800000;
const FDEF = 0xffc00000 | 0;

// a double's words at address p, into D
const dload = (m, p) => { DU[LO] = m.ld32(p); DU[HI] = m.ld32(p + 4); };
const dnan = (lo, hi) => (hi & 0x7fffffff) > 0x7ff00000 || ((hi & 0x7fffffff) === 0x7ff00000 && lo !== 0);
const dstore = (m, p, lo, hi) => { m.st32(p, lo | 0); m.st32(p + 4, hi | 0); };

// to a machine int: toward zero; what does not fit, the most negative
const toInt = v => (v >= -2147483648 && v < 2147483648) ? Math.trunc(v) | 0 : (0x80000000 | 0);

function farith(op, ua, ub) {
  if (fnan(ua)) return (ua | 0x00400000) | 0;
  if (op !== 5 && fnan(ub)) return (ub | 0x00400000) | 0;
  const a = fval(ua), b = fval(ub);
  let c;
  if (op === 1) c = a + b;
  else if (op === 2) c = a - b;
  else if (op === 3) c = a * b;
  else if (op === 4) c = a / b;
  else c = Math.sqrt(a);
  if (c !== c) return FDEF;
  return fbits(Math.fround(c));
}

function darith(m, op, pa, pb, pr) {
  dload(m, pa);
  const alo = DU[LO] | 0, ahi = DU[HI] | 0, a = D[0];
  if (dnan(alo, ahi)) return dstore(m, pr, alo, ahi | 0x00080000);
  let b = 0;
  if (op !== 21) {
    dload(m, pb);
    const blo = DU[LO] | 0, bhi = DU[HI] | 0;
    if (dnan(blo, bhi)) return dstore(m, pr, blo, bhi | 0x00080000);
    b = D[0];
  }
  let c;
  if (op === 17) c = a + b;
  else if (op === 18) c = a - b;
  else if (op === 19) c = a * b;
  else if (op === 20) c = a / b;
  else c = Math.sqrt(a);
  if (c !== c) return dstore(m, pr, 0, 0xfff80000);
  D[0] = c;
  dstore(m, pr, DU[LO], DU[HI]);
}

const cmp = (a, b) => (a !== a || b !== b) ? 2 : a < b ? -1 : a > b ? 1 : 0;

// Run one FPU instruction; M has ld32(addr) and st32(addr, v). Returns
// the accumulator, or undefined for a sub-op there is not.
export function fpu(m, sp) {
  const op = m.ld32(sp), a = m.ld32(sp + 4), b = m.ld32(sp + 8), r = m.ld32(sp + 12);
  switch (op) {
    case 1: case 2: case 3: case 4: case 5:
      return farith(op, a >>> 0, b >>> 0);
    case 6:
      return cmp(fval(a >>> 0), fval(b >>> 0));
    case 7:
      return fbits(Math.fround(a | 0));
    case 8:
      return toInt(fval(a >>> 0));
    case 17: case 18: case 19: case 20: case 21:
      darith(m, op, a, b, r);
      return 0;
    case 22: {
      dload(m, a); const x = D[0];
      dload(m, b); return cmp(x, D[0]);
    }
    case 23:
      D[0] = a | 0;
      dstore(m, r, DU[LO], DU[HI]);
      return 0;
    case 24:
      dload(m, a);
      return toInt(D[0]);
    case 25: {
      const u = a >>> 0;
      if (fnan(u)) {
        // the payload kept, quieted: softfp.c's __c4f_to_d
        const sig = u & 0x7fffff;
        dstore(m, r, (sig & 7) << 29, (u & 0x80000000) | 0x7ff80000 | (sig >>> 3));
      } else {
        D[0] = fval(u);
        dstore(m, r, DU[LO], DU[HI]);
      }
      return 0;
    }
    case 26: {
      dload(m, a);
      const lo = DU[LO], hi = DU[HI];
      if (dnan(lo | 0, hi | 0)) return ((hi & 0x80000000) | 0x7fc00000 | ((hi & 0xfffff) << 3) | (lo >>> 29)) | 0;
      return fbits(Math.fround(D[0]));
    }
    default:
      return undefined;
  }
}

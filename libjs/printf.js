// printf.js - the PRTF formatter.
//
// Native c4m passes PRTF straight to the host printf (c4m.c:2124-2143),
// so the reference is glibc's printf running on a 32-bit build
// (c4m32). Every argument is one 32-bit word. Length modifiers are
// accepted and mostly ignored, the same well-defined semantic the c4bb
// firmware and c4lm's stdio.h use; h and hh still truncate, as they do
// in glibc. The %lld tests are compared against 64-bit c4m for that
// reason (see tests/test-libjs.sh).
//
// Pure: bytes in, bytes out. The format string and %s arguments are
// read through a callback so this knows nothing about the arena.
//
//   format(fmt: Uint8Array, args: ArrayLike<int32>, str: addr => Uint8Array)
//     -> number[]   the bytes printf would have written

const f32 = new Float32Array(1), i32v = new Int32Array(f32.buffer);

const enc = s => { const a = []; for (let i = 0; i < s.length; i++) a.push(s.charCodeAt(i) & 0xff); return a; };

export function format(fmt, args, str) {
  const out = [];
  let ai = 0;
  const next = () => (ai < args.length ? args[ai++] | 0 : 0);
  let i = 0;
  const n = fmt.length;
  while (i < n) {
    const c = fmt[i++];
    if (c !== 37) { out.push(c); continue; }             // '%'
    if (i >= n) { out.push(37); break; }
    // flags
    let left = false, plus = false, space = false, zero = false, alt = false;
    for (;;) {
      const f = fmt[i];
      if (f === 45) left = true;                         // -
      else if (f === 43) plus = true;                    // +
      else if (f === 32) space = true;                   // ' '
      else if (f === 48) zero = true;                    // 0
      else if (f === 35) alt = true;                     // #
      else break;
      i++;
    }
    // width
    let width = 0;
    if (fmt[i] === 42) { width = next(); i++; if (width < 0) { left = true; width = -width; } }
    else while (fmt[i] >= 48 && fmt[i] <= 57) width = width * 10 + (fmt[i++] - 48);
    // precision
    let prec = -1;
    if (fmt[i] === 46) {
      i++;
      prec = 0;
      if (fmt[i] === 42) { prec = next(); i++; if (prec < 0) prec = -1; }
      else while (fmt[i] >= 48 && fmt[i] <= 57) prec = prec * 10 + (fmt[i++] - 48);
    }
    // length
    let len = 0;                                         // 0 none, 1 h, 2 hh
    for (;;) {
      const l = fmt[i];
      if (l === 104) { len = len === 1 ? 2 : 1; i++; }   // h, hh
      else if (l === 108 || l === 76 || l === 113 || l === 106 || l === 122 || l === 116) i++;
      else break;
    }
    const conv = fmt[i++];
    let body = [], prefix = [], numeric = false;
    switch (conv) {
      case 100: case 105: {                              // d i
        let v = next();
        if (len === 1) v = (v << 16) >> 16; else if (len === 2) v = (v << 24) >> 24;
        const neg = v < 0;
        body = digits(neg ? -v : v, 10, false, prec);
        if (neg) prefix = [45]; else if (plus) prefix = [43]; else if (space) prefix = [32];
        numeric = true;
        break;
      }
      case 117: case 120: case 88: case 111: {           // u x X o
        let v = next();
        if (len === 1) v &= 0xffff; else if (len === 2) v &= 0xff;
        v = v >>> 0;
        const base = conv === 111 ? 8 : conv === 117 ? 10 : 16;
        body = digits(v, base, conv === 88, prec);
        if (alt && v !== 0 && conv === 120) prefix = [48, 120];
        if (alt && v !== 0 && conv === 88) prefix = [48, 88];
        if (alt && conv === 111 && body[0] !== 48) body.unshift(48);
        numeric = true;
        break;
      }
      case 99:                                           // c
        body = [next() & 0xff];
        break;
      case 115: {                                        // s
        const p = next();
        if (!p) body = prec >= 0 && prec < 6 ? [] : enc('(null)');
        else {
          const s = str(p);
          body = Array.from(prec >= 0 && prec < s.length ? s.subarray(0, prec) : s);
        }
        break;
      }
      case 112: {                                        // p
        const p = next() >>> 0;
        body = p ? [48, 120, ...digits(p, 16, false, -1)] : enc('(nil)');
        break;
      }
      case 102: case 70: case 101: case 69: case 103: case 71: {  // f F e E g G
        // A word pushed where a double was expected. Read it as the
        // binary32 bit pattern c4m's FLT opcode uses, the one reading
        // of it that means anything.
        i32v[0] = next();
        const v = f32[0];
        const p = prec < 0 ? 6 : prec;
        let s;
        if (!isFinite(v)) s = isNaN(v) ? 'nan' : (v < 0 ? '-inf' : 'inf');
        else if (conv === 102 || conv === 70) s = v.toFixed(p);
        else if (conv === 101 || conv === 69) s = v.toExponential(p).replace(/e([+-])(\d)$/, 'e$10$2');
        else s = String(Number(v.toPrecision(p || 1)));
        if (conv === 70 || conv === 69 || conv === 71) s = s.toUpperCase();
        if (s[0] === '-') { prefix = [45]; s = s.slice(1); }
        else if (plus) prefix = [43]; else if (space) prefix = [32];
        body = enc(s);
        numeric = true;
        prec = -1;                                       // precision already applied
        break;
      }
      case 110: next(); continue;                        // n: consume, write nothing
      case 37: out.push(37); continue;                   // %%
      default:
        // Unknown conversion: glibc prints the directive as written.
        out.push(37);
        if (conv !== undefined) out.push(conv);
        continue;
    }
    const pad = width - body.length - prefix.length;
    const zeroPad = zero && !left && numeric && prec < 0;
    if (pad > 0 && !left && !zeroPad) for (let k = 0; k < pad; k++) out.push(32);
    for (const b of prefix) out.push(b);
    if (pad > 0 && zeroPad) for (let k = 0; k < pad; k++) out.push(48);
    for (const b of body) out.push(b);
    if (pad > 0 && left) for (let k = 0; k < pad; k++) out.push(32);
  }
  return out;
}

// Digits of a non-negative number with a minimum digit count. Precision
// 0 with a value of 0 prints nothing, as C requires.
function digits(v, base, upper, prec) {
  if (prec === 0 && v === 0) return [];
  let s = v.toString(base);
  if (upper) s = s.toUpperCase();
  if (prec > s.length) s = '0'.repeat(prec - s.length) + s;
  return enc(s);
}

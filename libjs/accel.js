// accel.js -- drawing accelerators: a game's innermost drawing loop run by
// the host, one instruction however many pixels (docs/accel.md).
//
// Numbers 0x1000 to 0x10ff (ACC_BASE to ACC_END, c4m.js) are these, outside
// the instruction set (0 up, append-only), C4KE's services (128 up) and
// C4IX's syscalls (200 up), so no compiler and no other machine has to know
// them. A program reaches one through OPCD alone,
//   __c4_opcode(args..., ACC_X)
// and only once INFO has said C4I_ACCEL (0x100000): a machine with accel on
// says so (createMachine's accel, on unless false), C4IX passes the bit to
// its tasks as it is, and everywhere else -- native c4m, c4bb, C4DOS, libjs
// with accel off -- the program draws with its own C loop. The guest's side
// is libjs/guest/accel.h. Each draws exactly the pixels its loop would.
//
// The arguments are at sp+4 upward, the last one nearest. Each reads only
// its arguments and what the guest could read, and writes only what the
// loop would have. Under the MMU every address goes through the task's
// pages (mmu.page, cached a page at a time here), and one it may not use
// faults as the loop's own load or store would: the instruction then runs
// again from the start once the kernel has seen to it, and drawing the same
// pixels twice draws the same picture. It costs one instruction's cycle.
import { STOP } from './c4m.js';
import { FAULT, FK } from './mmu.js';

export const ACC = {
  TCOL: 0x1000,     // Duke's walls, vlineasm1 and vlineasm4
  TCOLM: 0x1001,    // ... masked, mvlineasm1 and mvlineasm4
  HLINE: 0x1002,    // Duke's floors and ceilings, hlineasm4
  RHLINE: 0x1003,   // Duke's tilted screen, rhlineasm4
  SLOPE: 0x1004,    // Duke's sloped floors, slopevlin
  SPAN: 0x1010,     // Quake's texture spans, D_DrawSpans8's runs
  ZSPAN: 0x1011,    // Quake's z spans, D_DrawZSpans
  LBLOCK: 0x1012,   // Quake's surface blocks, R_DrawSurfaceBlock8_mip*
  SSPAN: 0x1013,    // Quake's sprites, D_SpriteDrawSpans's runs
};

// A byte reader, a byte writer and a word reader for one instruction:
// plain memory as runPlain's own loads take it, a task's pages under the
// MMU, or the machine's ld8 and st8 for anything else (the user window,
// the boot stack's guard, the low page, off the arena's end). Each keeps
// its own page, so a texture and a palette read in turn do not evict each
// other.
function reader(m) {
  const u8 = m.u8;
  if (m.vm) {
    const mmu = m.mmu;
    let vp = -1, base = 0;
    return x => {
      const v = x >>> 12;
      if (v !== vp) {
        if (v === 0) return m.ld8(x) & 255;
        const p = mmu.page(x, FK.READ);
        if (p < 0) throw FAULT;
        vp = v; base = p;
      }
      return u8[base | (x & 4095)];
    };
  }
  if (!m.uwin) {
    const hi = Math.min(m.guardLo(), u8.length);
    return x => (x >= 0x1000 && x < hi ? u8[x] : m.ld8(x) & 255);
  }
  return x => m.ld8(x) & 255;
}
function writer(m) {
  const u8 = m.u8;
  if (m.vm) {
    const mmu = m.mmu;
    let vp = -1, base = 0;
    return (x, b) => {
      const v = x >>> 12;
      if (v !== vp) {
        if (v === 0) { m.st8(x, b); return; }
        const p = mmu.page(x, FK.WRITE);
        if (p < 0) throw FAULT;
        vp = v; base = p;
      }
      u8[base | (x & 4095)] = b;
    };
  }
  if (!m.uwin) {
    const hi = Math.min(m.guardLo(), u8.length);
    return (x, b) => { if (x >= 0x1000 && x < hi) u8[x] = b; else m.st8(x, b); };
  }
  return (x, b) => m.st8(x, b);
}
function reader32(m) {
  const i32 = m.i32;
  if (!m.vm && !m.uwin) {
    const hi = Math.min(m.guardLo(), m.u8.length) - 3;
    return x => ((x & 3) === 0 && x >= 0x1000 && x < hi ? i32[x >> 2] | 0 : m.ld32(x) | 0);
  }
  return x => m.ld32(x) | 0;
}

// TCOL(count, v, inc, sh, tex, pal, dest, stride): count rows of
//   *dest = pal[tex[v >>> sh]]; v += inc; dest += stride;
// and a is v after the last. TCOLM leaves a pixel whose texel is 255 alone
// (a masked wall's hole).
function tcol(m, masked) {
  const sp = m.sp, w = k => m.ld32(sp + 4 * k) | 0;
  const stride = w(1), pal = w(3), tex = w(4), sh = w(5) & 31, inc = w(6);
  let dest = w(2), v = w(7), n = w(8);
  const rt = reader(m), rp = reader(m), wr = writer(m);
  if (masked) {
    for (; n > 0; n--, v = (v + inc) | 0, dest = (dest + stride) | 0) {
      const c = rt((tex + (v >>> sh)) | 0);
      if (c !== 255) wr(dest, rp((pal + c) | 0));
    }
  } else {
    for (; n > 0; n--, v = (v + inc) | 0, dest = (dest + stride) | 0)
      wr(dest, rp((pal + rt((tex + (v >>> sh)) | 0)) | 0));
  }
  m.a = v;
}

// HLINE(count, a, b, da, db, sh1, bits, sh2, tex, pal, dest): count pixels
// right to left from dest,
//   *dest-- = pal[tex[((a >>> sh1) << bits) | (b >>> sh2)]]; a -= da; b -= db;
// shifts taken modulo 32, as c4's SHL and LSR take them
function hline(m) {
  const sp = m.sp, w = k => m.ld32(sp + 4 * k) | 0;
  const pal = w(2), tex = w(3), sh2 = w(4), bits = w(5), sh1 = w(6), db = w(7), da = w(8);
  let dest = w(1), b = w(9), a = w(10), n = w(11);
  const rt = reader(m), rp = reader(m), wr = writer(m);
  for (; n > 0; n--, dest = (dest - 1) | 0, a = (a - da) | 0, b = (b - db) | 0)
    wr(dest, rp((pal + rt((tex + (((a >>> sh1) << bits) | (b >>> sh2))) | 0)) | 0));
  m.a = 0;
}

// RHLINE(n, tex, pal, p, c, s, x, y, da, db, ax, bx): n pixels right to left
// from p, the tilted screen's row as draw.c's rhlineasm4 has it, x and y
// kept with their sign bits flipped so the two borrows compare signed:
//   *p-- = pal[*tex]; tex -= c; if (y < bx) tex--; if (x < ax) tex -= s;
//   x -= da; y -= db;
function rhline(m) {
  const sp = m.sp, w = k => m.ld32(sp + 4 * k) | 0;
  const bx = w(1), ax = w(2), db = w(3), da = w(4), s = w(7), c = w(8), pal = w(10);
  let y = w(5), x = w(6), p = w(9), tex = w(11), n = w(12);
  const rt = reader(m), rp = reader(m), wr = writer(m);
  for (; n > 0; n--) {
    wr(p, rp((pal + rt(tex)) | 0));
    tex = (tex - c) | 0;
    if (y < bx) tex = (tex - 1) | 0;
    if (x < ax) tex = (tex - s) | 0;
    x = (x - da) | 0;
    y = (y - db) | 0;
    p = (p - 1) | 0;
  }
  m.a = 0;
}

// SLOPE(dest, z, pals, left, esi, edi, sh2, sh1, mask, bpl, tex, abits,
// stepbits, gx3, gy3, recip, pfpuasm, pasm4): draw.c's slopevlin from its
// first eight on, 1/z from a float's bits every eight pixels. abits and
// stepbits are floats' bits; a single's sum is Math.fround of the double
// one, as c4's FPU makes it. Leaves fpuasm and asm4 as the loop does.
const F = new Float32Array(1), FI = new Int32Array(F.buffer);
function slope(m) {
  const sp = m.sp, w = k => m.ld32(sp + 4 * k) | 0;
  const pasm4 = w(1), pfpu = w(2), recip = w(3), gy3 = w(4), gx3 = w(5);
  FI[0] = w(6); const step = F[0];
  FI[0] = w(7); let a = F[0];
  const tex = w(8), bpl = w(9), mask = w(10), sh1 = w(11) & 31, sh2 = w(12) & 31;
  let edi = w(13), esi = w(14), left = w(15), pals = w(16), z = w(17), dest = w(18);
  const rt = reader(m), rp = reader(m), wr = writer(m), r32 = reader32(m);
  let u = 0, asm4 = 0;
  do {
    F[0] = a; u = FI[0];
    const sgn = u >> 31;
    const k = (((u >> 23) & 255) - 2) & 31;
    const r = (r32((recip + 4 * ((u >> 12) & 0x7ff)) | 0) >>> k) ^ sgn;
    const d = (r - z) | 0;
    const xs = Math.imul(gx3, d) & ~0xff;
    let ystep = Math.imul(gy3, d);
    const yhi = ystep & ~0xff;
    z = r;
    a = Math.fround(a + step);
    asm4 = left;
    let n = left >= 8 ? 8 : (left & 0xff);
    for (; n !== 0; n--) {
      const bx = (esi >> sh2) & mask;
      esi = (esi + (xs | n)) | 0;
      const vy = (edi >>> sh1) | 0;
      edi = (edi + ystep) | 0;
      const pal = r32(pals);
      pals = (pals - 4) | 0;
      const pix = rp((pal + ((vy & ~0xff) | rt((tex + bx + vy) | 0))) | 0);
      ystep = yhi | pix;
      dest = (dest + bpl) | 0;
      wr(dest, pix);
    }
    left = (asm4 - 8) | 0;
  } while (left > 0);
  m.st32(pfpu, u);
  m.st32(pasm4, asm4);
  m.a = 0;
}

// SPAN(count, s, t, sstep, tstep, pbase, cachewidth, dest): count pixels
// left to right from dest, an affine run of a perspective span,
//   *dest++ = pbase[(s >> 16) + (t >> 16) * cachewidth]; s += sstep; t += tstep;
function span(m) {
  const sp = m.sp, w = k => m.ld32(sp + 4 * k) | 0;
  const cw = w(2), pbase = w(3), tstep = w(4), sstep = w(5);
  let dest = w(1), t = w(6), s = w(7), n = w(8);
  const rt = reader(m), wr = writer(m);
  for (; n > 0; n--, dest = (dest + 1) | 0, s = (s + sstep) | 0, t = (t + tstep) | 0)
    wr(dest, rt((pbase + (s >> 16) + Math.imul(t >> 16, cw)) | 0));
  m.a = 0;
}

// ZSPAN(count, izi, izistep, dest): count shorts from dest,
//   *dest++ = izi >> 16; izi += izistep;
function zspan(m) {
  const sp = m.sp, w = k => m.ld32(sp + 4 * k) | 0;
  const step = w(2);
  let dest = w(1), izi = w(3), n = w(4);
  const wr = writer(m);
  for (; n > 0; n--, dest = (dest + 2) | 0, izi = (izi + step) | 0) {
    const h = izi >> 16;
    wr(dest, h & 255);
    wr((dest + 1) | 0, (h >> 8) & 255);
  }
  m.a = 0;
}

// LBLOCK(rows, sh, ll, lr, lls, lrs, src, srcstep, cm, dest, deststep): a
// surface block, rows rows of rows pixels, each right to left,
//   step = (ll - lr) >> sh; light = lr;
//   for b = rows-1 .. 0: dest[b] = cm[(light & 0xFF00) + src[b]]; light += step;
//   src += srcstep; lr += lrs; ll += lls; dest += deststep;
function lblock(m) {
  const sp = m.sp, w = k => m.ld32(sp + 4 * k) | 0;
  const dstep = w(1), cm = w(3), sstep = w(4), src0 = w(5), lrs = w(6), lls = w(7), sh = w(10) & 31, rows = w(11);
  let dest = w(2), src = src0, lr = w(8), ll = w(9);
  const rs = reader(m), rc = reader(m), wr = writer(m);
  for (let i = 0; i < rows; i++) {
    const step = (ll - lr) >> sh;
    let light = lr;
    for (let b = rows - 1; b >= 0; b--) {
      wr((dest + b) | 0, rc((cm + (light & 0xff00) + rs((src + b) | 0)) | 0));
      light = (light + step) | 0;
    }
    src = (src + sstep) | 0;
    lr = (lr + lrs) | 0;
    ll = (ll + lls) | 0;
    dest = (dest + dstep) | 0;
  }
  m.a = 0;
}

// SSPAN(count, s, t, sstep, tstep, pbase, cachewidth, dest, pz, izi,
// izistep): a sprite's run, left to right, against the z buffer,
//   c = pbase[(s >> 16) + (t >> 16) * cachewidth];
//   if (c != 255 && *pz <= izi >> 16) { *pz = izi >> 16; *dest = c; }
//   izi += izistep; dest++; pz++; s += sstep; t += tstep;
// *pz a signed short; a is izi after the last. Run again after a fault it
// finds its own shorts equal, not less, and writes the same again.
function sspan(m) {
  const sp = m.sp, w = k => m.ld32(sp + 4 * k) | 0;
  const step = w(1), cw = w(5), pbase = w(6), tstep = w(7), sstep = w(8);
  let izi = w(2), pz = w(3), dest = w(4), t = w(9), s = w(10), n = w(11);
  const rt = reader(m), rz = reader(m), wz = writer(m), wr = writer(m);
  for (; n > 0; n--) {
    const c = rt((pbase + (s >> 16) + Math.imul(t >> 16, cw)) | 0);
    if (c !== 255) {
      const z = ((rz(pz) | (rz((pz + 1) | 0) << 8)) << 16) >> 16, h = izi >> 16;
      if (z <= h) {
        wz(pz, h & 255);
        wz((pz + 1) | 0, (h >> 8) & 255);
        wr(dest, c);
      }
    }
    izi = (izi + step) | 0;
    dest = (dest + 1) | 0;
    pz = (pz + 2) | 0;
    s = (s + sstep) | 0;
    t = (t + tstep) | 0;
  }
  m.a = izi;
}

// The machine's accel (c4m.js sys): run op, or -1 when it is not one of
// these, and the opcode is a custom one as any other number would be
export function accel(m, op) {
  switch (op) {
    case ACC.TCOL: tcol(m, false); break;
    case ACC.TCOLM: tcol(m, true); break;
    case ACC.HLINE: hline(m); break;
    case ACC.RHLINE: rhline(m); break;
    case ACC.SLOPE: slope(m); break;
    case ACC.SPAN: span(m); break;
    case ACC.ZSPAN: zspan(m); break;
    case ACC.LBLOCK: lblock(m); break;
    case ACC.SSPAN: sspan(m); break;
    default: return -1;
  }
  return STOP.BUDGET;
}

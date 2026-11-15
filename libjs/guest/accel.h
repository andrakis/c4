// accel.h -- the drawing accelerators, from inside the machine: a game's
// innermost drawing loop drawn by the host, one instruction however many
// pixels (libjs/accel.js, docs/accel.md).
//
// ANNOUNCED, NEVER PROBED: draw with one only once accel_present() has
// said so. On any other machine -- native c4m, c4bb, C4DOS, libjs with
// accel off -- the numbers are custom opcodes nobody installed, so a
// program keeps its own loop and draws with that there. Each accelerator
// draws exactly the pixels of the loop it stands for, in the same order.
// Only c4lc's builds can ask (__c4_inttypes__): built by another compiler,
// accel_present() is 0 and the calls are never made.
//
//   if (accel_present())
//       v = accel_tcol(count, v, inc, sh, tex, pal, dest, stride);
//   else
//       ... the loop ...
//
// The numbers 0x1000 to 0x10ff are the accelerators', outside the
// instruction set; the arguments are in accel.js's order.

#ifndef __C4_ACCEL_H
#define __C4_ACCEL_H 1

enum {
	C4I_ACCEL = 0x100000,
	ACC_TCOL = 0x1000, ACC_TCOLM = 0x1001, ACC_HLINE = 0x1002, ACC_RHLINE = 0x1003,
	ACC_SLOPE = 0x1004, ACC_SPAN = 0x1010, ACC_ZSPAN = 0x1011, ACC_LBLOCK = 0x1012,
	ACC_SSPAN = 0x1013
};

#ifdef __c4_inttypes__
#define accel_present() ((__c4_info() & C4I_ACCEL) != 0)
// a textured column, count rows down from dest: *dest = pal[tex[v >>> sh]];
// v += inc; dest += stride. Gives v after the last row. _masked leaves a
// pixel whose texel is 255 alone.
#define accel_tcol(n, v, inc, sh, tex, pal, dest, stride) \
	__c4_opcode((n), (v), (inc), (sh), (tex), (pal), (dest), (stride), ACC_TCOL)
#define accel_tcol_masked(n, v, inc, sh, tex, pal, dest, stride) \
	__c4_opcode((n), (v), (inc), (sh), (tex), (pal), (dest), (stride), ACC_TCOLM)
// a row right to left from dest: *dest-- = pal[tex[((a >>> sh1) << bits) |
// (b >>> sh2)]]; a -= da; b -= db
#define accel_hline(n, a, b, da, db, sh1, bits, sh2, tex, pal, dest) \
	__c4_opcode((n), (a), (b), (da), (db), (sh1), (bits), (sh2), (tex), (pal), (dest), ACC_HLINE)
// Duke's tilted screen's row (draw.c rhlineasm4)
#define accel_rhline(n, tex, pal, p, c, s, x, y, da, db, ax, bx) \
	__c4_opcode((n), (tex), (pal), (p), (c), (s), (x), (y), (da), (db), (ax), (bx), ACC_RHLINE)
// Duke's sloped floor's column (draw.c slopevlin), from its first eight on
#define accel_slope(dest, z, pals, left, esi, edi, sh2, sh1, mask, bpl, tex, abits, stepbits, gx3, gy3, recip, pfpuasm, pasm4) \
	__c4_opcode((dest), (z), (pals), (left), (esi), (edi), (sh2), (sh1), (mask), (bpl), (tex), (abits), \
		(stepbits), (gx3), (gy3), (recip), (pfpuasm), (pasm4), ACC_SLOPE)
// an affine run left to right from dest: *dest++ = pbase[(s >> 16) +
// (t >> 16) * width]; s += sstep; t += tstep
#define accel_span(n, s, t, sstep, tstep, pbase, width, dest) \
	__c4_opcode((n), (s), (t), (sstep), (tstep), (pbase), (width), (dest), ACC_SPAN)
// n shorts from dest: *dest++ = izi >> 16; izi += izistep
#define accel_zspan(n, izi, izistep, dest) \
	__c4_opcode((n), (izi), (izistep), (dest), ACC_ZSPAN)
// Quake's surface block: rows rows of rows pixels, each right to left with
// light from lr stepping (ll - lr) >> sh, dest[b] = cm[(light & 0xFF00) +
// src[b]]; then src, lr, ll and dest step by srcstep, lrs, lls, deststep
#define accel_lblock(rows, sh, ll, lr, lls, lrs, src, srcstep, cm, dest, deststep) \
	__c4_opcode((rows), (sh), (ll), (lr), (lls), (lrs), (src), (srcstep), (cm), (dest), (deststep), ACC_LBLOCK)
// a sprite's run against the z buffer: c = pbase[(s >> 16) + (t >> 16) *
// width]; if (c != 255 && *pz <= izi >> 16) { *pz = izi >> 16; *dest = c; };
// izi += izistep, dest, pz, s and t stepping. Gives izi after the last.
#define accel_sspan(n, s, t, sstep, tstep, pbase, width, dest, pz, izi, izistep) 	__c4_opcode((n), (s), (t), (sstep), (tstep), (pbase), (width), (dest), (pz), (izi), (izistep), ACC_SSPAN)
#else
#define accel_present() 0
#define accel_tcol(n, v, inc, sh, tex, pal, dest, stride) 0
#define accel_tcol_masked(n, v, inc, sh, tex, pal, dest, stride) 0
#define accel_hline(n, a, b, da, db, sh1, bits, sh2, tex, pal, dest) 0
#define accel_rhline(n, tex, pal, p, c, s, x, y, da, db, ax, bx) 0
#define accel_slope(dest, z, pals, left, esi, edi, sh2, sh1, mask, bpl, tex, abits, stepbits, gx3, gy3, recip, pfpuasm, pasm4) 0
#define accel_span(n, s, t, sstep, tstep, pbase, width, dest) 0
#define accel_zspan(n, izi, izistep, dest) 0
#define accel_lblock(rows, sh, ll, lr, lls, lrs, src, srcstep, cm, dest, deststep) 0
#define accel_sspan(n, s, t, sstep, tstep, pbase, width, dest, pz, izi, izistep) 0
#endif

#endif

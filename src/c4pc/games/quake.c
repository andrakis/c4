/* quake.c -- Quake's accelerators (games/quake/c4/accel.mjs) in C: the
 * renderer's innermost loops. source/c4accel.h is the game's side (QA_*).
 * The floats as c4's FPU computes them, as accel.mjs has them: each single
 * operation the double one rounded (fr, Math.fround), a double's in
 * doubles, a conversion to int toward zero with what does not fit the most
 * negative int (toint). */
#include "../accel.h"

/* a span list's espan_t: u, v, count, pnext */
#define SPAN_U 0
#define SPAN_V 4
#define SPAN_COUNT 8
#define SPAN_NEXT 12

/* D_DrawSpans8 and Turbulent8's perspective, every 8 or 16 pixels (n, its
 * shift): the whole list. gradients points at the nine floats d_sdivzstepu,
 * d_tdivzstepu, d_zistepu, d_sdivzstepv, d_tdivzstepv, d_zistepv,
 * d_sdivzorigin, d_tdivzorigin, d_ziorigin. */
static i32 spans(machine *m, int turb) {
  i32 a[10]; acc_args(m, 10, a);
  i32 pspan = a[0], pbase = a[1], vb = a[2], sw = a[3], cw = turb ? 0 : a[4];
  i32 sadjust = turb ? a[4] : a[5], tadjust = turb ? a[5] : a[6], bbs = turb ? a[6] : a[7], bbt = turb ? a[7] : a[8];
  i32 tb = turb ? a[8] : 0, grad = a[9];
  const i32 N = turb ? 16 : 8, SH = turb ? 4 : 3;
  RW(r32); RW(rt); RW(wr); RW(rturb);
#define G(k) fval(rd32(&r32, grad + 4 * (k)))
  double su = G(0), tu = G(1), zu = G(2), sv = G(3), tv = G(4), zv = G(5), so = G(6), to = G(7), zo = G(8);
#undef G
  double suN = fr(su * N), tuN = fr(tu * N), zuN = fr(zu * N);
  i32 sstep = 0, tstep = 0;
  do {
    i32 pu = rd32(&r32, pspan + SPAN_U), pv = rd32(&r32, pspan + SPAN_V);
    i32 dest = vb + IMUL(sw, pv) + pu;
    i32 count = rd32(&r32, pspan + SPAN_COUNT);
    double du = fr(pu), dv = fr(pv);
    double sdivz = fr(fr(so + fr(dv * sv)) + fr(du * su));
    double tdivz = fr(fr(to + fr(dv * tv)) + fr(du * tu));
    double zi = fr(fr(zo + fr(dv * zv)) + fr(du * zu));
    double z = fr(65536 / zi);
    i32 s = toint(fr(sdivz * z)) + sadjust;
    if (s > bbs) s = bbs; else if (s < 0) s = 0;
    i32 t = toint(fr(tdivz * z)) + tadjust;
    if (t > bbt) t = bbt; else if (t < 0) t = 0;
    do {
      i32 n = count >= N ? N : count;
      count -= n;
      i32 snext, tnext;
      if (count) {
        sdivz = fr(sdivz + suN); tdivz = fr(tdivz + tuN); zi = fr(zi + zuN);
        z = fr(65536 / zi);
        snext = toint(fr(sdivz * z)) + sadjust;
        if (snext > bbs) snext = bbs; else if (snext < N) snext = N;
        tnext = toint(fr(tdivz * z)) + tadjust;
        if (tnext > bbt) tnext = bbt; else if (tnext < N) tnext = N;
        sstep = (snext - s) >> SH;
        tstep = (tnext - t) >> SH;
      } else {
        double m1 = fr(n - 1);
        sdivz = fr(sdivz + fr(su * m1)); tdivz = fr(tdivz + fr(tu * m1)); zi = fr(zi + fr(zu * m1));
        z = fr(65536 / zi);
        snext = toint(fr(sdivz * z)) + sadjust;
        if (snext > bbs) snext = bbs; else if (snext < N) snext = N;
        tnext = toint(fr(tdivz * z)) + tadjust;
        if (tnext > bbt) tnext = bbt; else if (tnext < N) tnext = N;
        if (n > 1) {
          sstep = (i32)(u32)(((i64)snext - s) / (n - 1));
          tstep = (i32)(u32)(((i64)tnext - t) / (n - 1));
        }
      }
      if (!turb) {
        for (i32 k = n; k > 0; k--, dest++, s += sstep, t += tstep)
          wr8(&wr, dest, rd8(&rt, pbase + (s >> 16) + IMUL(t >> 16, cw)));
      } else {
        i32 rs = s & 0x7fffff, rtt = t & 0x7fffff;      /* (CYCLE<<16)-1 */
        for (i32 k = n; k > 0; k--, dest++) {
          i32 st = ((rs + rd32(&rturb, tb + 4 * ((rtt >> 16) & 127))) >> 16) & 63;
          i32 tt = ((rtt + rd32(&rturb, tb + 4 * ((rs >> 16) & 127))) >> 16) & 63;
          wr8(&wr, dest, rd8(&rt, pbase + (tt << 6) + st));
          rs += sstep;
          rtt += tstep;
        }
      }
      s = snext;
      t = tnext;
    } while (count > 0);
    pspan = rd32(&r32, pspan + SPAN_NEXT);
  } while (pspan != 0);
  return 0;
}
/* QA_SPANS8 (pspan, pbase, viewbuffer, screenwidth, cachewidth, sadjust,
 * tadjust, bbextents, bbextentt, gradients) */
static i32 spans8(machine *m) { return spans(m, 0); }
/* QA_TURB8 (pspan, pbase, viewbuffer, screenwidth, sadjust, tadjust,
 * bbextents, bbextentt, turb, gradients): water and lava */
static i32 turb8(machine *m) { return spans(m, 1); }

/* QA_ZSPANS (pspan, zbuffer, zwidth, gradients): D_DrawZSpans, 1/z as shorts */
static i32 zspans(machine *m) {
  i32 a[4]; acc_args(m, 4, a);
  i32 pspan = a[0], zbuf = a[1], zw = a[2], grad = a[3];
  RW(r32); RW(wr);
  double zu = fval(rd32(&r32, grad + 8)), zv = fval(rd32(&r32, grad + 20)), zo = fval(rd32(&r32, grad + 32));
  i32 izistep = toint(fr(fr(zu * 32768) * 65536));
  do {
    i32 pu = rd32(&r32, pspan + SPAN_U), pv = rd32(&r32, pspan + SPAN_V);
    i32 dest = zbuf + 2 * (IMUL(zw, pv) + pu);
    i32 n = rd32(&r32, pspan + SPAN_COUNT);
    double zi = fr(fr(zo + fr(fr(pv) * zv)) + fr(fr(pu) * zu));
    i32 izi = toint(zi * 32768 * 65536);
    for (; n > 0; n--, dest += 2, izi += izistep) {
      i32 h = izi >> 16;
      wr8(&wr, dest, h & 255);
      wr8(&wr, dest + 1, (h >> 8) & 255);
    }
    pspan = rd32(&r32, pspan + SPAN_NEXT);
  } while (pspan != 0);
  return 0;
}

/* QA_LBLOCK (rows, sh, ll, lr, lls, lrs, src, srcstep, cm, dest, deststep):
 * R_DrawSurfaceBlock8_mip0..3's block */
static i32 lblock(machine *m) {
  i32 a[11]; acc_args(m, 11, a);
  i32 rows = a[0], sh = a[1] & 31, ll = a[2], lr = a[3], lls = a[4], lrs = a[5], src = a[6], sstep = a[7], cm = a[8],
      dest = a[9], dstep = a[10];
  RW(rs); RW(rc); RW(wr);
  for (i32 i = 0; i < rows; i++) {
    i32 step = (ll - lr) >> sh, light = lr;
    for (i32 b = rows - 1; b >= 0; b--) {
      wr8(&wr, dest + b, rd8(&rc, cm + (light & 0xff00) + rd8(&rs, src + b)));
      light += step;
    }
    src += sstep;
    lr += lrs;
    ll += lls;
    dest += dstep;
  }
  return 0;
}

static inline i32 zget(rw_t *r, i32 p) { return (i32)((u32)(rd8(r, p) | rd8(r, p + 1) << 8) << 16) >> 16; }

/* QA_SSPAN (count, s, t, sstep, tstep, pbase, cachewidth, dest, pz, izi,
 * izistep): a sprite's run against the z buffer; gives izi after */
static i32 sspan(machine *m) {
  i32 a[11]; acc_args(m, 11, a);
  i32 n = a[0], s = a[1], t = a[2], sstep = a[3], tstep = a[4], pbase = a[5], cw = a[6], dest = a[7], pz = a[8],
      izi = a[9], step = a[10];
  RW(rt); RW(rz); RW(wz); RW(wr);
  for (; n > 0; n--) {
    i32 c = rd8(&rt, pbase + (s >> 16) + IMUL(t >> 16, cw));
    if (c != 255) {
      i32 z = zget(&rz, pz), h = izi >> 16;
      if (z <= h) {
        wr8(&wz, pz, h & 255);
        wr8(&wz, pz + 1, (h >> 8) & 255);
        wr8(&wr, dest, c);
      }
    }
    izi += step;
    dest++;
    pz += 2;
    s += sstep;
    t += tstep;
  }
  return izi;
}

/* QA_POLYSPANS (pspanpackage, paspancount, perrorterm, erroradjustup,
 * erroradjustdown, countextrastep, ubasestep, acolormap, zistepx, lstepx,
 * ststepxwhole, sstepxfrac, tstepxfrac, skinwidth): D_PolysetDrawSpans8 */
static i32 polyspans(machine *m) {
  i32 a[14]; acc_args(m, 14, a);
  i32 pk = a[0], pasp = a[1], perr = a[2], up = a[3], down = a[4], extra = a[5], ubase = a[6], cmap = a[7], zix = a[8],
      lx = a[9], whole = a[10], sfr = a[11], tfr = a[12], skinw = a[13];
  RW(r32); RW(w32); RW(rt); RW(rc); RW(rz); RW(wz); RW(wr);
  i32 asp = rd32(&r32, pasp), err = rd32(&r32, perr);
  do {
    i32 lcount = asp - rd32(&r32, pk + 8);
    err += up;
    if (err >= 0) { asp += extra; err -= down; }
    else asp += ubase;
    if (lcount) {
      i32 dest = rd32(&r32, pk), pz = rd32(&r32, pk + 4), ptex = rd32(&r32, pk + 12);
      i32 sfrac = rd32(&r32, pk + 16), tfrac = rd32(&r32, pk + 20), light = rd32(&r32, pk + 24), zi = rd32(&r32, pk + 28);
      do {
        i32 h = zi >> 16;
        if (h >= zget(&rz, pz)) {
          wr8(&wr, dest, rd8(&rc, cmap + rd8(&rt, ptex) + (light & 0xff00)));
          wr8(&wz, pz, h & 255);
          wr8(&wz, pz + 1, (h >> 8) & 255);
        }
        dest++;
        zi += zix;
        pz += 2;
        light += lx;
        ptex += whole;
        sfrac += sfr;
        ptex += sfrac >> 16;
        sfrac &= 0xffff;
        tfrac += tfr;
        if (tfrac & 0x10000) { ptex += skinw; tfrac &= 0xffff; }
        lcount--;
      } while (lcount);
    }
    pk += 32;
  } while (rd32(&r32, pk + 8) != -999999);
  wr32(&w32, pasp, asp);
  wr32(&w32, perr, err);
  return 0;
}

/* QA_PARTICLES (first, prm): R_DrawParticles once its dead are freed: each
 * particle drawn as D_DrawParticle draws it, and moved as the loop moves
 * it, the moves written once every particle's page is known to be there.
 * particle_t: org 0, color 12, next 16, vel 20, ramp 32, die 36, type 40. */
static i32 particles(machine *m) {
  i32 a[2]; acc_args(m, 2, a);
  i32 first = a[0], prm = a[1];
  RW(r32); RW(w32); RW(rz); RW(wz); RW(wr);
#define F(k) fval(rd32(&r32, prm + 4 * (k)))
#define N(k) rd32(&r32, prm + 4 * (k))
  double ox = F(0), oy = F(1), oz = F(2), rx = F(3), ry = F(4), rzz = F(5);
  double ux = F(6), uy = F(7), uz = F(8), px = F(9), py = F(10), pzz = F(11);
  double xc = F(12), yc = F(13), ft = F(14), t1 = F(15), t2 = F(16), t3 = F(17), grav = F(18), dvel = F(19);
  i32 vbottom = N(20), vright = N(21), vy = N(22), vx = N(23), zbuf = N(24), zw = N(25), vbuf = N(26), scan = N(27);
  i32 pshift = N(28), pmin = N(29), pmax = N(30), yshift = N(31), sw = N(32), ramp1 = N(33), ramp2 = N(34), ramp3 = N(35);
#undef F
#undef N
  u32 nm = 0;
  i32 *moves = acc_scratch(0, 10 * 64);
  for (i32 p = first; p != 0; p = rd32(&r32, p + 16)) {
    acc_probe(m, p, 44, 1);
    double o0 = fval(rd32(&r32, p)), o1 = fval(rd32(&r32, p + 4)), o2 = fval(rd32(&r32, p + 8));
    double color = fval(rd32(&r32, p + 12));
    /* D_DrawParticle */
    double l0 = fr(o0 - ox), l1 = fr(o1 - oy), l2 = fr(o2 - oz);
    double tx = fr(fr(fr(l0 * rx) + fr(l1 * ry)) + fr(l2 * rzz));
    double ty = fr(fr(fr(l0 * ux) + fr(l1 * uy)) + fr(l2 * uz));
    double tz = fr(fr(fr(l0 * px) + fr(l1 * py)) + fr(l2 * pzz));
    if (!(tz < 8)) {
      double zi = fr(1 / tz);
      i32 u = toint(fr(xc + fr(zi * tx)) + 0.5), v = toint(fr(yc - fr(zi * ty)) + 0.5);
      if (!(v > vbottom || u > vright || v < vy || u < vx)) {
        i32 pz = zbuf + 2 * (IMUL(zw, v) + u);
        i32 pd = vbuf + rd32(&r32, scan + 4 * v) + u;
        i32 izi = toint(fr(zi * 32768));
        i32 pix = SAR(izi, pshift);
        if (pix < pmin) pix = pmin; else if (pix > pmax) pix = pmax;
        i32 c = toint(color) & 255;
        for (i32 count = SHL(pix, yshift); count; count--, pz += 2 * zw, pd += sw) {
          for (i32 i = 0; i < pix; i++) {
            i32 at = pz + 2 * i;
            if (zget(&rz, at) <= izi) {
              wr8(&wz, at, izi & 255);
              wr8(&wz, at + 1, (izi >> 8) & 255);
              wr8(&wr, pd + i, c);
            }
          }
        }
      }
    }
    /* the move, written below */
    double v0 = fval(rd32(&r32, p + 20)), v1 = fval(rd32(&r32, p + 24)), v2 = fval(rd32(&r32, p + 28));
    double ramp = fval(rd32(&r32, p + 32));
    i32 die = rd32(&r32, p + 36), col = rd32(&r32, p + 12);
    i32 type = rd32(&r32, p + 40);
    double n0 = fr(o0 + fr(v0 * ft)), n1 = fr(o1 + fr(v1 * ft)), n2 = fr(o2 + fr(v2 * ft));
#define RAMPTO(step, top, table) do { \
      ramp = fr(ramp + (step)); \
      if (ramp >= (top)) die = fbits(-1); \
      else col = fbits(fr(rd32(&r32, (table) + 4 * toint(ramp)))); \
    } while (0)
    switch (type) {
      case 0: break;                                                   /* pt_static */
      case 3: RAMPTO(t1, 6, ramp3); v2 = fr(v2 + grav); break;         /* pt_fire */
      case 4: RAMPTO(t2, 8, ramp1);                                    /* pt_explode */
        v0 = fr(v0 + fr(v0 * dvel)); v1 = fr(v1 + fr(v1 * dvel)); v2 = fr(v2 + fr(v2 * dvel));
        v2 = fr(v2 - grav); break;
      case 5: RAMPTO(t3, 8, ramp2);                                    /* pt_explode2 */
        v0 = fr(v0 - fr(v0 * ft)); v1 = fr(v1 - fr(v1 * ft)); v2 = fr(v2 - fr(v2 * ft));
        v2 = fr(v2 - grav); break;
      case 6:                                                          /* pt_blob */
        v0 = fr(v0 + fr(v0 * dvel)); v1 = fr(v1 + fr(v1 * dvel)); v2 = fr(v2 + fr(v2 * dvel));
        v2 = fr(v2 - grav); break;
      case 7:                                                          /* pt_blob2 */
        v0 = fr(v0 - fr(v0 * dvel)); v1 = fr(v1 - fr(v1 * dvel));
        v2 = fr(v2 - grav); break;
      case 1: case 2: v2 = fr(v2 - grav); break;                       /* pt_grav, pt_slowgrav */
    }
#undef RAMPTO
    moves = acc_scratch(0, 10 * (nm + 1));
    i32 *mv = moves + 10 * nm++;
    mv[0] = p; mv[1] = fbits(n0); mv[2] = fbits(n1); mv[3] = fbits(n2); mv[4] = col;
    mv[5] = fbits(v0); mv[6] = fbits(v1); mv[7] = fbits(v2); mv[8] = fbits(ramp); mv[9] = die;
  }
  for (u32 k = 0; k < nm; k++) {
    i32 *mv = moves + 10 * k, p = mv[0];
    wr32(&w32, p, mv[1]); wr32(&w32, p + 4, mv[2]); wr32(&w32, p + 8, mv[3]);
    wr32(&w32, p + 12, mv[4]);
    wr32(&w32, p + 20, mv[5]); wr32(&w32, p + 24, mv[6]); wr32(&w32, p + 28, mv[7]);
    wr32(&w32, p + 32, mv[8]); wr32(&w32, p + 36, mv[9]);
  }
  return 0;
}

/* QA_LMFILL (bl, size, ambient, lightmap, nmaps, adj): R_BuildLightMap's
 * first two loops */
static i32 lmfill(machine *m) {
  i32 a[6]; acc_args(m, 6, a);
  i32 bl = a[0], size = a[1], ambient = a[2], lm = a[3], nmaps = a[4], adj = a[5];
  RW(r32); RW(w32); RW(rl);
  for (i32 i = 0; i < size; i++) wr32(&w32, bl + 4 * i, ambient);
  for (i32 k = 0, at = lm; k < nmaps; k++, at += size) {
    i32 scale = rd32(&r32, adj + 4 * k);
    for (i32 i = 0; i < size; i++) {
      i32 x = bl + 4 * i;
      wr32(&w32, x, rd32(&r32, x) + IMUL(rd8(&rl, at + i), scale));
    }
  }
  return 0;
}

/* ToInt32 of a float's value (|x| >= 2^63 is a multiple of 2^40: 0) */
static i32 toint32(double x) {
  if (!(x > -9.2e18 && x < 9.2e18)) return 0;
  return (i32)(u32)(u64)(i64)x;
}
/* x >> 1 of a number JavaScript keeps whole (2147483648 from -INT_MIN) */
static inline i64 half(i64 x) { return (i32)(u32)x >> 1; }

/* QA_DLIGHT (bl, smax, tmax, prm): R_AddDynamicLights' texels for one light */
static i32 dlight(machine *m) {
  i32 a[4]; acc_args(m, 4, a);
  i32 bl = a[0], smax = a[1], tmax = a[2], prm = a[3];
  RW(r32); RW(w32);
  double l0 = fval(rd32(&r32, prm)), l1 = fval(rd32(&r32, prm + 4)), rad = fval(rd32(&r32, prm + 8)),
         minl = fval(rd32(&r32, prm + 12));
  acc_probe(m, bl, IMUL(IMUL(4, smax), tmax), 1);
  for (i32 t = 0; t < tmax; t++) {
    i64 td = toint(fr(l1 - fr((double)t * 16)));
    if (td < 0) td = -td;
    for (i32 s = 0; s < smax; s++) {
      i64 sd = toint(fr(l0 - fr((double)s * 16)));
      if (sd < 0) sd = -sd;
      double dist = fr((double)(sd > td ? sd + half(td) : td + half(sd)));
      if (dist < minl) {
        i32 x = bl + 4 * (IMUL(t, smax) + s);
        double sum = fr(fr((double)(u32)rd32(&r32, x)) + fr(fr(rad - dist) * 256));
        wr32(&w32, x, sum < 2147483648.0 ? toint(sum) : toint32(sum));
      }
    }
  }
  return 0;
}

/* QA_LMFIN (bl, size, shift): R_BuildLightMap's last loop */
static i32 lmfin(machine *m) {
  i32 a[3]; acc_args(m, 3, a);
  i32 bl = a[0], size = a[1], shift = a[2];
  RW(r32); RW(w32);
  acc_probe(m, bl, IMUL(4, size), 1);
  for (i32 i = 0; i < size; i++) {
    i32 x = bl + 4 * i;
    i32 t = SAR(65280 - rd32(&r32, x), shift);
    if (t < 64) t = 64;
    wr32(&w32, x, t);
  }
  return 0;
}

/* QA_ALIASVERTS (fv, st, pv, n, prm): R_AliasTransformAndProjectFinalVerts */
static i32 aliasverts(machine *m) {
  i32 a[5]; acc_args(m, 5, a);
  i32 fv = a[0], st = a[1], pv = a[2], n = a[3], prm = a[4];
  RW(r32); RW(w32); RW(rb);
#define F(k) fval(rd32(&r32, prm + 4 * (k)))
  double t00 = F(0), t01 = F(1), t02 = F(2), t03 = F(3), t10 = F(4), t11 = F(5), t12 = F(6), t13 = F(7);
  double t20 = F(8), t21 = F(9), t22 = F(10), t23 = F(11), xc = F(12), yc = F(13);
  double l0 = F(14), l1 = F(15), l2 = F(16), shade = F(17);
#undef F
  i32 ambient = rd32(&r32, prm + 72), normals = rd32(&r32, prm + 76);
  for (i32 i = 0; i < n; i++, fv += 32, pv += 4, st += 12) {
    double b0 = rd8(&rb, pv), b1 = rd8(&rb, pv + 1), b2 = rd8(&rb, pv + 2);
    i32 li = rd8(&rb, pv + 3);
    double zi = fr(1 / fr(fr(fr(fr(b0 * t20) + fr(b1 * t21)) + fr(b2 * t22)) + t23));
    wr32(&w32, fv + 20, toint(zi));
    wr32(&w32, fv, toint(fr(fr(fr(fr(fr(fr(b0 * t00) + fr(b1 * t01)) + fr(b2 * t02)) + t03) * zi) + xc)));
    wr32(&w32, fv + 4, toint(fr(fr(fr(fr(fr(fr(b0 * t10) + fr(b1 * t11)) + fr(b2 * t12)) + t13) * zi) + yc)));
    wr32(&w32, fv + 8, rd32(&r32, st + 4));
    wr32(&w32, fv + 12, rd32(&r32, st + 8));
    wr32(&w32, fv + 24, rd32(&r32, st));
    i32 nv = normals + 12 * li;
    double lightcos = fr(fr(fr(fval(rd32(&r32, nv)) * l0) + fr(fval(rd32(&r32, nv + 4)) * l1)) + fr(fval(rd32(&r32, nv + 8)) * l2));
    i32 temp = ambient;
    if (lightcos < 0) {
      temp += toint(fr(shade * lightcos));
      if (temp < 0) temp = 0;
    }
    wr32(&w32, fv + 16, temp);
  }
  return 0;
}

/* QA_POLYTRI (p1, p2, p3, prm): D_PolysetRecursiveTriangle */
typedef struct { machine *m; rw_t r32, rz, rs, rc, wz, wr; i32 zspan, skin, cmap, scan, vb; } tri_t;
static inline int far_(i64 d) { return d < -1 || d > 1; }
static void tri(tri_t *T, const i32 *p1, const i32 *p2, const i32 *p3) {
  for (;;) {
    if (far_((i64)p2[0] - p1[0]) || far_((i64)p2[1] - p1[1])) break;
    if (far_((i64)p3[0] - p2[0]) || far_((i64)p3[1] - p2[1])) { const i32 *t = p1; p1 = p2; p2 = p3; p3 = t; break; }
    if (far_((i64)p1[0] - p3[0]) || far_((i64)p1[1] - p3[1])) { const i32 *t = p1; p1 = p3; p3 = p2; p2 = t; break; }
    return;                       /* the whole triangle is drawn */
  }
  i32 nw[6] = {(p1[0] + p2[0]) >> 1, (p1[1] + p2[1]) >> 1, (p1[2] + p2[2]) >> 1, (p1[3] + p2[3]) >> 1, 0,
               (p1[5] + p2[5]) >> 1};
  /* the point, if this is a leading edge */
  if (!(p2[1] > p1[1]) && !(p2[1] == p1[1] && p2[0] < p1[0])) {
    i32 z = nw[5] >> 16;
    i32 zb = rd32(&T->r32, T->zspan + 4 * nw[1]) + 2 * nw[0];
    if (z >= zget(&T->rz, zb)) {
      wr8(&T->wz, zb, z & 255);
      wr8(&T->wz, zb + 1, (z >> 8) & 255);
      i32 pix = rd8(&T->rc, T->cmap + rd8(&T->rs, rd32(&T->r32, T->skin + 4 * (nw[3] >> 16)) + (nw[2] >> 16)));
      wr8(&T->wr, T->vb + rd32(&T->r32, T->scan + 4 * nw[1]) + nw[0], pix);
    }
  }
  tri(T, p3, p1, nw);
  tri(T, p3, nw, p2);
}
static i32 polytri(machine *m) {
  i32 a[4]; acc_args(m, 4, a);
  tri_t T; T.m = m;
  rw_init(&T.r32, m); rw_init(&T.rz, m); rw_init(&T.rs, m); rw_init(&T.rc, m); rw_init(&T.wz, m); rw_init(&T.wr, m);
  i32 prm = a[3];
  T.zspan = rd32(&T.r32, prm); T.skin = rd32(&T.r32, prm + 4); T.cmap = rd32(&T.r32, prm + 8);
  T.scan = rd32(&T.r32, prm + 12); T.vb = rd32(&T.r32, prm + 16);
  i32 v[3][6];
  for (int k = 0; k < 3; k++) {
    i32 p = a[k];
    v[k][0] = rd32(&T.r32, p); v[k][1] = rd32(&T.r32, p + 4); v[k][2] = rd32(&T.r32, p + 8);
    v[k][3] = rd32(&T.r32, p + 12); v[k][4] = 0; v[k][5] = rd32(&T.r32, p + 20);
  }
  tri(&T, v[0], v[1], v[2]);
  return 0;
}

/* QA_SINCOS (pa, ps, pc): (float)sin(a) and (float)cos(a), with the FPU
 * opcode's sin and cos (the x87's on the PC, libm's on Linux) where libjs
 * has the browser's: they may differ in a double's last bit */
static i32 sincos_(machine *m) {
  i32 a[3]; acc_args(m, 3, a);
  RW(r32); RW(w32);
  double x = fval(rd32(&r32, a[0]));
  wr32(&w32, a[1], fbits(fr(fpu_sin(x))));
  wr32(&w32, a[2], fbits(fr(fpu_cos(x))));
  return 0;
}

const acc_fn acc_quake[] = {spans8, zspans, turb8, lblock, sspan, polyspans, particles, lmfill, dlight, lmfin,
                            aliasverts, polytri, sincos_};
const int acc_quake_n = sizeof acc_quake / sizeof acc_quake[0];

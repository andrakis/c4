// math.c -- C4IX libc's <math.h> (docs/c4-float.md), in double precision
// on top of c4lc's float support.
//
// The exact functions are exact: sqrt is softfp's correctly rounded one,
// and floor, ceil, trunc, round, fmod, modf, frexp and ldexp work on the
// bits. The rest follow fdlibm (Sun Microsystems, 1993: "Permission to
// use, copy, modify, and distribute this software is freely granted,
// provided that this notice is preserved."), whose algorithms and
// coefficients these are: Cody-Waite argument reduction and minimax
// polynomials, within about an ulp of the true value; pow is e_pow.c, and
// sin, cos and tan reduce arguments past 2^20 pi/2 by Payne-Hanek, with
// the bits of 2/pi. tests/mathtest.c measures them all against glibc.
//
// The float versions (sinf, ...) are the double ones rounded.
//
// No syscalls, nothing from the rest of libc but errno: the tests
// compile this file alone.

extern int errno;

static unsigned long long m_bits(double x) { return *(unsigned long long *)&x; }
static double m_from(unsigned long long b) { return *(double *)&b; }
static int m_exp(double x) { return (int)(m_bits(x) >> 52) & 2047; }
static int m_neg(double x) { return (int)(m_bits(x) >> 63); }

double fabs(double x) { return m_from(m_bits(x) & 0x7FFFFFFFFFFFFFFFull); }
double copysign(double x, double y) {
    return m_from((m_bits(x) & 0x7FFFFFFFFFFFFFFFull) | (m_bits(y) & 0x8000000000000000ull));
}
int __c4ix_isnan(double x) { return (m_bits(x) & 0x7FFFFFFFFFFFFFFFull) > 0x7FF0000000000000ull; }
int __c4ix_isinf(double x) { return (m_bits(x) & 0x7FFFFFFFFFFFFFFFull) == 0x7FF0000000000000ull; }
int __c4ix_isfinite(double x) { return m_exp(x) != 2047; }

double sqrt(double x) { return m_from(__c4d_sqrt(m_bits(x))); }

// ---- by the bits ----

double trunc(double x) {
    int e;
    unsigned long long b, m;
    b = m_bits(x);
    e = m_exp(x) - 1023;
    if (e >= 52) return x;                       // already whole, inf, NaN
    if (e < 0) return m_from(b & 0x8000000000000000ull);
    m = 0x000FFFFFFFFFFFFFull >> e;
    return m_from(b & ~m);
}
double floor(double x) {
    double t;
    t = trunc(x);
    if (m_neg(x) && t != x) return t - 1;
    return t;
}
double ceil(double x) {
    double t;
    t = trunc(x);
    if (!m_neg(x) && t != x) return t + 1;
    return t;
}
// halfway away from zero
double round(double x) {
    double t;
    t = trunc(x);
    if (fabs(x - t) >= 0.5) t = t + (m_neg(x) ? -1.0 : 1.0);
    return t;
}
double modf(double x, double *ip) {
    double t;
    t = trunc(x);
    *ip = t;
    if (__c4ix_isinf(x)) return copysign(0.0, x);
    return copysign(x - t, x);
}

// x * 2^n, rounded once (musl's scalbn)
double ldexp(double x, int n) {
    if (n > 1023) {
        x = x * m_from(0x7FE0000000000000ull);  n = n - 1023;
        if (n > 1023) { x = x * m_from(0x7FE0000000000000ull); n = n - 1023; if (n > 1023) n = 1023; }
    } else if (n < 0 - 1022) {
        // 2^-969: down, but not so far that the last step rounds twice
        x = x * m_from(0x0360000000000000ull); n = n + 969;
        if (n < 0 - 1022) { x = x * m_from(0x0360000000000000ull); n = n + 969; if (n < 0 - 1022) n = 0 - 1022; }
    }
    return x * m_from((unsigned long long)(1023 + n) << 52);
}
double scalbn(double x, int n) { return ldexp(x, n); }

double frexp(double x, int *ep) {
    int e;
    unsigned long long b;
    b = m_bits(x);
    e = m_exp(x);
    if (e == 2047 || (b << 1) == 0) { *ep = 0; return x; }
    if (!e) {                                    // subnormal: normalise first
        x = x * m_from(0x4350000000000000ull);  // 2^54
        b = m_bits(x);
        e = m_exp(x) - 54;
    }
    *ep = e - 1022;
    return m_from((b & 0x800FFFFFFFFFFFFFull) | 0x3FE0000000000000ull);
}

// x - n*y, n = trunc(x/y), exactly: the significands as integers, the
// remainder taken a bit at a time from the top
double fmod(double x, double y) {
    unsigned long long bx, by, mx, my;
    int ex, ey, sx;
    bx = m_bits(x); by = m_bits(y);
    sx = m_neg(x);
    ex = m_exp(x); ey = m_exp(y);
    if (ex == 2047 || ey == 2047 || (by << 1) == 0) {
        if (ey == 2047 && (by << 12) == 0 && ex != 2047) return x;  // finite % inf
        return (x * y) / (x * y);                                    // NaN
    }
    if ((bx << 1) <= (by << 1)) {
        if ((bx << 1) == (by << 1)) return copysign(0.0, x);
        return x;
    }
    mx = bx & 0x000FFFFFFFFFFFFFull;
    my = by & 0x000FFFFFFFFFFFFFull;
    if (ex) mx = mx | 0x0010000000000000ull; else ex = 1;
    if (ey) my = my | 0x0010000000000000ull; else ey = 1;
    while (ex > ey) {
        if (mx >= my) mx = mx - my;
        mx = mx << 1;
        --ex;
    }
    if (mx >= my) mx = mx - my;
    if (!mx) return copysign(0.0, x);
    while (!(mx >> 52)) { mx = mx << 1; --ex; }
    if (ex > 0) mx = (mx & 0x000FFFFFFFFFFFFFull) | ((unsigned long long)ex << 52);
    else mx = mx >> (1 - ex);
    return m_from(mx | ((unsigned long long)sx << 63));
}

double fmin(double a, double b) { if (__c4ix_isnan(a)) return b; if (__c4ix_isnan(b)) return a; return a < b ? a : b; }
double fmax(double a, double b) { if (__c4ix_isnan(a)) return b; if (__c4ix_isnan(b)) return a; return a > b ? a : b; }

// ---- exp and log ----

static double ln2_hi = 6.93147180369123816490e-01;
static double ln2_lo = 1.90821492927058770002e-10;
static double invln2 = 1.44269504088896338700e+00;
static double P1 = 1.66666666666666019037e-01;
static double P2 = -2.77777777770155933842e-03;
static double P3 = 6.61375632143793436117e-05;
static double P4 = -1.65339022054652515390e-06;
static double P5 = 4.13813679705723846039e-08;

// exp by a table, as musl and FreeBSD do theirs: x = (32m + j) ln2/32 + r,
// |r| <= ln2/64, and exp(x) = 2^m * 2^(j/32) * exp(r), the table entry a
// double and its tail (tests/exptab.py). exp(r) - 1 is small, so the one
// rounding that matters is the last add: within about 0.52 ulp, as
// glibc's is, and the two differ in about one case in 200 -- where
// fdlibm's polynomial over all of ln2 differed in one in ten (exp(1)
// among them).
static unsigned long long m_e2t[64] = {
    0x3FF0000000000000ull, 0x0000000000000000ull, 0x3FF059B0D3158574ull, 0x3C8D73E2A475B465ull,
    0x3FF0B5586CF9890Full, 0x3C98A62E4ADC610Bull, 0x3FF11301D0125B51ull, 0xBC96C51039449B3Aull,
    0x3FF172B83C7D517Bull, 0xBC819041B9D78A76ull, 0x3FF1D4873168B9AAull, 0x3C9E016E00A2643Cull,
    0x3FF2387A6E756238ull, 0x3C99B07EB6C70573ull, 0x3FF29E9DF51FDEE1ull, 0x3C8612E8AFAD1255ull,
    0x3FF306FE0A31B715ull, 0x3C86F46AD23182E4ull, 0x3FF371A7373AA9CBull, 0xBC963AEABF42EAE2ull,
    0x3FF3DEA64C123422ull, 0x3C8ADA0911F09EBCull, 0x3FF44E086061892Dull, 0x3C489B7A04EF80D0ull,
    0x3FF4BFDAD5362A27ull, 0x3C7D4397AFEC42E2ull, 0x3FF5342B569D4F82ull, 0xBC807ABE1DB13CADull,
    0x3FF5AB07DD485429ull, 0x3C96324C054647ADull, 0x3FF6247EB03A5585ull, 0xBC9383C17E40B497ull,
    0x3FF6A09E667F3BCDull, 0xBC9BDD3413B26456ull, 0x3FF71F75E8EC5F74ull, 0xBC816E4786887A99ull,
    0x3FF7A11473EB0187ull, 0xBC841577EE04992Full, 0x3FF82589994CCE13ull, 0xBC9D4C1DD41532D8ull,
    0x3FF8ACE5422AA0DBull, 0x3C96E9F156864B27ull, 0x3FF93737B0CDC5E5ull, 0xBC675FC781B57EBCull,
    0x3FF9C49182A3F090ull, 0x3C7C7C46B071F2BEull, 0x3FFA5503B23E255Dull, 0xBC9D2F6EDB8D41E1ull,
    0x3FFAE89F995AD3ADull, 0x3C97A1CD345DCC81ull, 0x3FFB7F76F2FB5E47ull, 0xBC75584F7E54AC3Bull,
    0x3FFC199BDD85529Cull, 0x3C811065895048DDull, 0x3FFCB720DCEF9069ull, 0x3C7503CBD1E949DBull,
    0x3FFD5818DCFBA487ull, 0x3C82ED02D75B3707ull, 0x3FFDFC97337B9B5Full, 0xBC91A5CD4F184B5Cull,
    0x3FFEA4AFA2A490DAull, 0xBC9E9C23179C2893ull, 0x3FFF50765B6E4540ull, 0x3C99D3E12DD8A18Bull };

double exp(double x) {
    double fk, rhi, rlo, r, t, p, thi, tlo;
    int k, j;
    if (__c4ix_isnan(x)) return x + x;
    if (x > 709.782712893383973096) { errno = 34; return m_from(0x7FF0000000000000ull); }
    if (x < -745.13321910194110842) { errno = 34; return 0.0; }
    if (fabs(x) < 5.551115123125783e-17) return 1.0 + x;
    // k = x 32/ln2, nearest; r = x - k ln2/32 as r + t, ln2/32's top part
    // short enough that k times it is exact
    fk = trunc(x * m_from(0x40471547652B82FEull) + (m_neg(x) ? -0.5 : 0.5));
    k = (int)fk;
    rhi = x - fk * m_from(0x3F962E42FEF00000ull);
    rlo = fk * m_from(0x3D8473DE6AF278EDull);
    r = rhi - rlo;
    t = (rhi - r) - rlo;
    // exp(r) - 1 by Taylor: the next term is 4e-18 of the answer
    p = r + r * r * (0.5 + r * (0.16666666666666666 + r * (0.041666666666666664 +
        r * (0.008333333333333333 + r * 0.001388888888888889))));
    p = p + t * (1.0 + p);
    j = k & 31;
    thi = *(double *)&m_e2t[2 * j];
    tlo = *(double *)&m_e2t[2 * j + 1];
    return ldexp(thi + (tlo + thi * p), (k - j) / 32);
}

static double Lg1 = 6.666666666666735130e-01;
static double Lg2 = 3.999999999940941908e-01;
static double Lg3 = 2.857142874366239149e-01;
static double Lg4 = 2.222219843214978396e-01;
static double Lg5 = 1.818357216161805012e-01;
static double Lg6 = 1.531383769920937332e-01;
static double Lg7 = 1.479819860511658591e-01;

// log(x) as k*ln2 + log(1+f), sqrt(2)/2 < 1+f < sqrt(2); K, F, f*f/2 and
// the polynomial's part out too, for log10 and log2
static int m_lk;
static double m_lf, m_lh, m_lr;
double log(double x) {
    unsigned long long b;
    int k;
    double f, s, z, w, t1, t2, R, hfsq, m;
    b = m_bits(x);
    if (__c4ix_isnan(x)) return x + x;
    if ((b << 1) == 0) { errno = 34; return m_from(0xFFF0000000000000ull); }
    if (m_neg(x)) { errno = 33; return (x - x) / 0.0; }
    if (__c4ix_isinf(x)) return x;
    m = frexp(x, &k);                    // x = m 2^k, 0.5 <= m < 1
    if (m < 0.70710678118654752440) { m = m * 2; --k; }
    f = m - 1.0;
    m_lk = k; m_lf = f;
    s = f / (2.0 + f);
    z = s * s;
    w = z * z;
    t1 = w * (Lg2 + w * (Lg4 + w * Lg6));
    t2 = z * (Lg1 + w * (Lg3 + w * (Lg5 + w * Lg7)));
    R = t2 + t1;
    hfsq = 0.5 * f * f;
    m_lh = hfsq;
    m_lr = s * (hfsq + R);
    return k * ln2_hi - ((hfsq - (m_lr + k * ln2_lo)) - f);
}

static unsigned int m_hi(double x) { return (unsigned int)(m_bits(x) >> 32); }
static unsigned int m_lo(double x) { return (unsigned int)m_bits(x); }
static double m_hl(unsigned int h, unsigned int l) { return m_from(((unsigned long long)h << 32) | l); }
static double m_lo0(double x) { return m_from(m_bits(x) & 0xFFFFFFFF00000000ull); }

// log10 and log2 as fdlibm (FreeBSD's): log(1+f) as HI + LO, HI with 21
// bits, so that HI times each constant's top half is exact
static double m_lhi, m_llo;
static int m_lsplit(double x) {
    double l;
    l = log(x);
    if (!__c4ix_isfinite(l) || x <= 0) { m_lhi = l; return 0; }
    m_lhi = m_lo0(m_lf - m_lh);
    m_llo = (m_lf - m_lhi) - m_lh + m_lr;
    return 1;
}
double log10(double x) {
    double vh, vl, y2, w;
    if (!m_lsplit(x)) return m_lhi;
    vh = m_lhi * 4.34294481878168880939e-01;
    y2 = m_lk * 3.01029995663611771306e-01;
    vl = m_lk * 3.69423907715893078616e-13 + (m_llo + m_lhi) * 2.50829467116452752298e-11 + m_llo * 4.34294481878168880939e-01;
    w = y2 + vh;
    vl = vl + ((y2 - w) + vh);
    return vl + w;
}
double log2(double x) {
    double vh, vl, y, w;
    if (!m_lsplit(x)) return m_lhi;
    vh = m_lhi * 1.44269504072144627571e+00;
    vl = (m_llo + m_lhi) * 1.67517131648865118353e-10 + m_llo * 1.44269504072144627571e+00;
    y = m_lk;
    w = y + vh;
    vl = vl + ((y - w) + vh);
    return vl + w;
}
double log1p(double x) {
    double u;
    u = 1.0 + x;
    if (u == 1.0) return x;
    return log(u) * (x / (u - 1.0));      // Goldberg's correction
}
double expm1(double x) {
    double u;
    if (fabs(x) < 1e-5) return x + 0.5 * x * x + x * x * x / 6;
    u = exp(x);
    if (u == 1.0) return x;
    if (u - 1.0 == -1.0) return -1.0;
    return (u - 1.0) * x / log(u);        // Kahan's
}

static int m_isint(double y) { return __c4ix_isfinite(y) && trunc(y) == y; }
static int m_isodd(double y) { return m_isint(y) && fabs(y) < 9007199254740992.0 && fmod(y, 2.0) != 0; }

// pow is fdlibm's e_pow.c: log2(x) in two parts to about 2^-64, times
// y in two parts, then 2^z, so within about an ulp for every y
static double bp[2] = { 1.0, 1.5 };
static double dp_h[2] = { 0.0, 5.84962487220764160156e-01 };
static double dp_l[2] = { 0.0, 1.35003920212974897128e-08 };
static double pw_L1 = 5.99999999999994648725e-01;
static double pw_L2 = 4.28571428578550184252e-01;
static double pw_L3 = 3.33333329818377432918e-01;
static double pw_L4 = 2.72728123808534006489e-01;
static double pw_L5 = 2.30660745775561754067e-01;
static double pw_L6 = 2.06975017800338417784e-01;
static double pw_lg2 = 6.93147180559945286227e-01;
static double pw_lg2_h = 6.93147182464599609375e-01;
static double pw_lg2_l = -1.90465429995776804525e-09;
static double pw_ovt = 8.0085662595372944372e-17;
static double pw_cp = 9.61796693925975554329e-01;
static double pw_cp_h = 9.61796700954437255859e-01;
static double pw_cp_l = -7.02846165095275826516e-09;
static double pw_ivln2 = 1.44269504088896338700e+00;
static double pw_ivln2_h = 1.44269502162933349609e+00;
static double pw_ivln2_l = 1.92596299112661746887e-08;

static double m_over(double s) { errno = 34; return s * 1e300 * 1e300; }
static double m_under(double s) { errno = 34; return s * 1e-300 * 1e-300; }

// x > 0 finite, y finite and not 0; S the sign of the answer
static double m_pow(double ax, double y, double s) {
    double z, z_h, z_l, p_h, p_l, y1, t1, t2, r, t, u, v, w, ss, s2, s_h, s_l, t_h, t_l;
    unsigned int ix, iy, j, i, hz;
    int k, n;
    ix = m_hi(ax);
    iy = m_hi(y) & 0x7FFFFFFF;
    if (iy > 0x41E00000) {                          // |y| > 2^31
        if (iy > 0x43F00000) {                      // |y| > 2^64: over or under
            if (ix <= 0x3FEFFFFF) return m_neg(y) ? m_over(s) : m_under(s);
            return m_neg(y) ? m_under(s) : m_over(s);
        }
        if (ix < 0x3FEFFFFF) return m_neg(y) ? m_over(s) : m_under(s);
        if (ix > 0x3FF00000) return m_neg(y) ? m_under(s) : m_over(s);
        // |1 - x| <= 2^-20: log(x) by x - x^2/2 + x^3/3 - x^4/4
        t = ax - 1.0;
        w = (t * t) * (0.5 - t * (0.3333333333333333333333 - t * 0.25));
        u = pw_ivln2_h * t;
        v = t * pw_ivln2_l - w * pw_ivln2;
        t1 = m_lo0(u + v);
        t2 = v - (t1 - u);
    } else {
        n = 0;
        if (ix < 0x00100000) { ax = ax * 9007199254740992.0; n = n - 53; ix = m_hi(ax); }
        n = n + (int)(ix >> 20) - 1023;
        j = ix & 0x000FFFFF;
        ix = j | 0x3FF00000;
        if (j <= 0x3988E) k = 0;                    // |x| < sqrt(3/2)
        else if (j < 0xBB67A) k = 1;                // |x| < sqrt(3)
        else { k = 0; n = n + 1; ix = ix - 0x00100000; }
        ax = m_hl(ix, m_lo(ax));
        // ss = s_h + s_l = (x - 1)/(x + 1) or (x - 1.5)/(x + 1.5)
        u = ax - bp[k];
        v = 1.0 / (ax + bp[k]);
        ss = u * v;
        s_h = m_lo0(ss);
        t_h = m_hl(((ix >> 1) | 0x20000000) + 0x00080000 + ((unsigned int)k << 18), 0);
        t_l = ax - (t_h - bp[k]);
        s_l = v * ((u - s_h * t_h) - s_h * t_l);
        s2 = ss * ss;
        r = s2 * s2 * (pw_L1 + s2 * (pw_L2 + s2 * (pw_L3 + s2 * (pw_L4 + s2 * (pw_L5 + s2 * pw_L6)))));
        r = r + s_l * (s_h + ss);
        s2 = s_h * s_h;
        t_h = m_lo0(3.0 + s2 + r);
        t_l = r - ((t_h - 3.0) - s2);
        u = s_h * t_h;
        v = s_l * t_h + t_l * ss;
        p_h = m_lo0(u + v);
        p_l = v - (p_h - u);
        z_h = pw_cp_h * p_h;
        z_l = pw_cp_l * p_h + p_l * pw_cp + dp_l[k];
        // log2(ax) = n + dp_h + z_h + z_l = t1 + t2
        t = (double)n;
        t1 = m_lo0(((z_h + z_l) + dp_h[k]) + t);
        t2 = z_l - (((t1 - t) - dp_h[k]) - z_h);
    }
    // y = y1 + y2; z = y * log2(x) = p_h + p_l
    y1 = m_lo0(y);
    p_l = (y - y1) * t1 + y * t2;
    p_h = y1 * t1;
    z = p_l + p_h;
    j = m_hi(z);
    i = m_lo(z);
    if (!m_neg(z) && j >= 0x40900000) {             // z >= 1024
        if (((j - 0x40900000) | i) != 0) return m_over(s);
        if (p_l + pw_ovt > z - p_h) return m_over(s);
    } else if (m_neg(z) && (j & 0x7FFFFFFF) >= 0x4090CC00) {    // z <= -1075
        if (((j - 0xC090CC00) | i) != 0) return m_under(s);
        if (p_l <= z - p_h) return m_under(s);
    }
    // 2^(p_h + p_l), n the nearest integer to z when |z| > 0.5
    i = j & 0x7FFFFFFF;
    k = (int)(i >> 20) - 1023;
    n = 0;
    if (i > 0x3FE00000) {
        hz = j + (0x00100000 >> (k + 1));
        k = (int)((hz & 0x7FFFFFFF) >> 20) - 1023;
        t = m_hl((hz >> (20 - k)) << (20 - k), 0);
        n = (int)(((hz & 0x000FFFFF) | 0x00100000) >> (20 - k));
        if (m_neg(z)) n = 0 - n;
        p_h = p_h - t;
    }
    t = m_lo0(p_l + p_h);
    u = t * pw_lg2_h;
    v = (p_l - (t - p_h)) * pw_lg2 + t * pw_lg2_l;
    z = u + v;
    w = v - (z - u);
    t = z * z;
    t1 = z - t * (P1 + t * (P2 + t * (P3 + t * (P4 + t * P5))));
    r = (z * t1) / (t1 - 2.0) - (w + z * w);
    z = 1.0 - (r - z);
    j = m_hi(z);
    if ((int)((j >> 20) & 2047) + n <= 0) z = ldexp(z, n);     // a subnormal answer
    else z = m_hl(j + ((unsigned int)n << 20), m_lo(z));
    return s * z;
}

double pow(double x, double y) {
    int neg;
    if (y == 0) return 1.0;
    if (x == 1.0) return 1.0;
    if (__c4ix_isnan(x) || __c4ix_isnan(y)) return x + y;
    if (__c4ix_isinf(y)) {
        if (x == -1.0) return 1.0;
        if ((fabs(x) < 1) == (y < 0)) return y < 0 ? -y : y;
        return 0.0;
    }
    if (x == 0) {
        if (y < 0) { errno = 34; return m_isodd(y) ? copysign(m_from(0x7FF0000000000000ull), x) : m_from(0x7FF0000000000000ull); }
        return m_isodd(y) ? x : 0.0;
    }
    if (__c4ix_isinf(x)) {
        if (m_neg(x)) return pow(-0.0, -y);
        return y < 0 ? 0.0 : x;
    }
    if (y == 1.0) return x;
    if (y == 2.0) return x * x;
    if (y == -1.0) return 1.0 / x;
    if (y == 0.5 && x > 0) return sqrt(x);
    neg = 0;
    if (x < 0) {
        if (!m_isint(y)) { errno = 33; return (x - x) / (x - x); }
        neg = m_isodd(y);
        x = -x;
    }
    return m_pow(x, y, neg ? -1.0 : 1.0);
}

double cbrt(double x) {
    double r;
    if (x == 0 || !__c4ix_isfinite(x)) return x;
    r = pow(fabs(x), 1.0 / 3.0);
    r = r - (r * r * r - fabs(x)) / (3 * r * r);   // a Newton step
    return copysign(r, x);
}

double hypot(double x, double y) {
    double a, b, t;
    a = fabs(x); b = fabs(y);
    if (__c4ix_isinf(a) || __c4ix_isinf(b)) return m_from(0x7FF0000000000000ull);
    if (a < b) { t = a; a = b; b = t; }
    if (a == 0) return 0.0;
    t = b / a;
    return a * sqrt(1 + t * t);
}

// ---- sin, cos, tan ----

static double S1 = -1.66666666666666324348e-01;
static double S2 = 8.33333333332248946124e-03;
static double S3 = -1.98412698298579493134e-04;
static double S4 = 2.75573137070700676789e-06;
static double S5 = -2.50507602534068634195e-08;
static double S6 = 1.58969099521155010221e-10;
static double C1 = 4.16666666666666019037e-02;
static double C2 = -1.38888888888741095749e-03;
static double C3 = 2.48015872894767294178e-05;
static double C4 = -2.75573143513906633035e-07;
static double C5 = 2.08757232129817482790e-09;
static double C6 = -1.13596475577881948265e-11;

// sin and cos of x + y, |x + y| <= pi/4, y the tail of the reduction
static double k_sin(double x, double y) {
    double z, v, r;
    z = x * x;
    v = z * x;
    r = S2 + z * (S3 + z * (S4 + z * (S5 + z * S6)));
    return x - ((z * (0.5 * y - v * r) - y) - v * S1);
}
static double k_cos(double x, double y) {
    double z, r, hz, w;
    z = x * x;
    r = z * (C1 + z * (C2 + z * (C3 + z * (C4 + z * (C5 + z * C6)))));
    hz = 0.5 * z;
    w = 1.0 - hz;
    return w + (((1.0 - w) - hz) + (z * r - x * y));
}

// x = n pi/2 + (r + rt), |r + rt| <= pi/4: the quadrant N out, R and RT
static double m_r, m_rt;

// Payne-Hanek, for |x| past 2^20 pi/2: the bits of 2/pi (tests/twopi.py
// makes them), the window of 192 of them that matters for x's exponent
// times x's 53-bit significand, exactly, in 32-bit limbs
static unsigned int m_2pi[40] = {
    0xA2F9836E, 0x4E441529, 0xFC2757D1, 0xF534DDC0, 0xDB629599, 0x3C439041,
    0xFE5163AB, 0xDEBBC561, 0xB7246E3A, 0x424DD2E0, 0x06492EEA, 0x09D1921C,
    0xFE1DEB1C, 0xB129A73E, 0xE88235F5, 0x2EBB4484, 0xE99C7026, 0xB45F7E41,
    0x3991D639, 0x835339F4, 0x9C845F8B, 0xBDF9283B, 0x1FF897FF, 0xDE05980F,
    0xEF2F118B, 0x5A0A6D1F, 0x6D367ECF, 0x27CB09B7, 0x4F463F66, 0x9E5FEA2D,
    0x7527BAC7, 0xEBE5F17B, 0x3D0739F7, 0x8A5292EA, 0x6BFB5FB1, 0x1F8D5D08,
    0x56033046, 0xFC7B6BAB, 0xF0CFBC20, 0x9AF4361D };
static unsigned int m_p[8];

static unsigned int m_2piw(int q) { return (q < 0 || q >= 40) ? 0 : m_2pi[q]; }
// bits POS .. POS+31 of 2/pi's fraction, counting from 1; none before 1
static unsigned int m_2pibits(int pos) {
    int pp, q, r;
    pp = pos - 1 + 64;
    q = pp / 32 - 2;
    r = pp % 32;
    if (!r) return m_2piw(q);
    return ((m_2piw(q) << r) & 0xFFFFFFFF) | (m_2piw(q + 1) >> (32 - r));
}
// the 32 bits of m_p whose top one is bit TOP; none below bit 0
static unsigned int m_pw(int top) {
    int lo, q, r;
    unsigned int v;
    if (top < 0) return 0;
    lo = top - 31;
    if (lo < 0) return (m_p[0] << (0 - lo)) & 0xFFFFFFFF;
    q = lo >> 5;
    r = lo & 31;
    v = m_p[q] >> r;
    if (r && q < 7) v = v | ((m_p[q + 1] << (32 - r)) & 0xFFFFFFFF);
    return v;
}
static void m_padd(int at, unsigned long long t) {
    unsigned long long s;
    s = (unsigned long long)m_p[at] + (t & 0xFFFFFFFFull);
    m_p[at] = (unsigned int)s;
    s = (unsigned long long)m_p[at + 1] + (t >> 32) + (s >> 32);
    m_p[at + 1] = (unsigned int)s;
    at = at + 2;
    while ((s >> 32) && at < 8) {
        s = (unsigned long long)m_p[at] + 1;
        m_p[at] = (unsigned int)s;
        ++at;
    }
}
// a * b + the rounding error of it, exactly (Dekker): M_TP the error
static double m_tp;
static double m_twoprod(double a, double b) {
    double p, c, ah, al, bh, bl;
    p = a * b;
    c = 134217729.0 * a; ah = c - (c - a); al = a - ah;
    c = 134217729.0 * b; bh = c - (c - b); bl = b - bh;
    m_tp = ((ah * bh - p) + ah * bl + al * bh) + al * bl;
    return p;
}
static int m_reduce_big(double x) {
    unsigned long long m, b;
    unsigned int ml, mh, hw, lw;
    int e, i0, k, n, h, neg;
    double d1, d2, p, lo;
    m = (m_bits(x) & 0x000FFFFFFFFFFFFFull) | 0x0010000000000000ull;
    e = m_exp(x) - 1075;                        // |x| = m 2^e
    ml = (unsigned int)m;
    mh = (unsigned int)(m >> 32);
    // the bits of 2/pi before E - 1 only add multiples of 4 to x 2/pi;
    // the 192 from there leave 190 bits of fraction in the product
    i0 = e - 1;
    k = 0;
    while (k < 8) { m_p[k] = 0; ++k; }
    k = 0;
    while (k < 6) {
        b = m_2pibits(i0 + 32 * (5 - k));
        m_padd(k, (unsigned long long)ml * b);
        m_padd(k + 1, (unsigned long long)mh * b);
        ++k;
    }
    n = (int)(m_p[5] >> 30) & 3;
    neg = 0;
    if ((m_p[5] >> 29) & 1) {                    // fraction >= 1/2: next quadrant, 1 - fraction
        ++n;
        neg = 1;
        b = 1;
        k = 0;
        while (k < 6) {
            b = (unsigned long long)(0xFFFFFFFF ^ m_p[k]) + b;
            m_p[k] = (unsigned int)b;
            b = b >> 32;
            ++k;
        }
    }
    m_p[5] = m_p[5] & 0x3FFFFFFF;
    // the fraction is m_p's bits 189 .. 0: its top bit, then 53 + 52 bits
    h = 189;
    while (h > 0 && !((m_pw(h) >> 31) & 1)) --h;
    hw = m_pw(h);
    lw = m_pw(h - 32) >> 11;
    d1 = m_from(((unsigned long long)(1023 + h - 190) << 52) | ((unsigned long long)(hw & 0x7FFFFFFF) << 21) | lw);
    hw = m_pw(h - 53) >> 12;                     // 20 bits
    lw = m_pw(h - 73);                           // 32 bits
    d2 = m_from(0x4330000000000000ull | ((unsigned long long)hw << 32) | lw) - 4503599627370496.0;
    d2 = ldexp(d2, h - 104 - 190);
    // r = fraction * pi/2, as a double and its tail
    p = m_twoprod(d1, 1.57079632679489655800e+00);
    lo = m_tp + (d1 * 6.12323399573676603587e-17 + d2 * 1.57079632679489655800e+00);
    m_r = p + lo;
    m_rt = lo - (m_r - p);
    if (neg) { m_r = -m_r; m_rt = -m_rt; }
    if (m_neg(x)) { m_r = -m_r; m_rt = -m_rt; n = 4 - n; }
    return n & 3;
}

static int m_reduce(double x) {
    double fn, r, w, t;
    int n;
    if (fabs(x) <= 0.785398163397448279) { m_r = x; m_rt = 0; return 0; }
    if (fabs(x) >= 1647099.0) return m_reduce_big(x);
    fn = trunc(x * 6.36619772367581382433e-01 + (m_neg(x) ? -0.5 : 0.5));
    n = (int)fmod(fn, 4.0);
    if (n < 0) n = n + 4;
    // pi/2 in three parts, each exact when multiplied by fn
    r = x - fn * 1.57079632673412561417e+00;
    w = fn * 6.07710050650619224932e-11;
    t = r;
    w = fn * 6.07710050630396597660e-11;
    r = t - w;
    w = fn * 2.02226624879595063154e-21 - ((t - r) - w);
    t = r;
    w = fn * 2.02226624871116645580e-21;
    r = t - w;
    w = fn * 8.47842766036889956997e-32 - ((t - r) - w);
    m_r = r - w;
    m_rt = (r - m_r) - w;
    return n;
}
double sin(double x) {
    int n;
    if (!__c4ix_isfinite(x)) return x - x;
    n = m_reduce(x);
    if (n == 0) return k_sin(m_r, m_rt);
    if (n == 1) return k_cos(m_r, m_rt);
    if (n == 2) return -k_sin(m_r, m_rt);
    return -k_cos(m_r, m_rt);
}
double cos(double x) {
    int n;
    if (!__c4ix_isfinite(x)) return x - x;
    n = m_reduce(x);
    if (n == 0) return k_cos(m_r, m_rt);
    if (n == 1) return -k_sin(m_r, m_rt);
    if (n == 2) return -k_cos(m_r, m_rt);
    return k_sin(m_r, m_rt);
}
double tan(double x) {
    int n;
    double s, c;
    if (!__c4ix_isfinite(x)) return x - x;
    n = m_reduce(x);
    s = k_sin(m_r, m_rt);
    c = k_cos(m_r, m_rt);
    if (n & 1) return -c / s;
    return s / c;
}

// ---- atan, atan2, asin, acos ----

static double atanhi[4] = { 4.63647609000806093515e-01, 7.85398163397448278999e-01,
    9.82793723247329054082e-01, 1.57079632679489655800e+00 };
static double atanlo[4] = { 2.26987774529616870924e-17, 3.06161699786838301793e-17,
    1.39033110312309984516e-17, 6.12323399573676603587e-17 };
static double aT[11] = { 3.33333333333329318027e-01, -1.99999999998764832476e-01,
    1.42857142725034663711e-01, -1.11111104054623557880e-01, 9.09088713343650656196e-02,
    -7.69187620504482999495e-02, 6.66107313738753120669e-02, -5.83357013379057348645e-02,
    4.97687799461593236017e-02, -3.65315727442169155270e-02, 1.62858201153657823623e-02 };

double atan(double x) {
    double a, z, w, s1, s2;
    int id;
    if (__c4ix_isnan(x)) return x + x;
    a = fabs(x);
    if (a >= 7.378697629483820646e19) return copysign(atanhi[3] + atanlo[3], x);
    if (a < 0.4375) {
        if (a < 7.450580596923828125e-09) return x;
        id = -1;
    } else if (a < 1.1875) {
        if (a < 0.6875) { id = 0; a = (2.0 * a - 1.0) / (2.0 + a); }
        else { id = 1; a = (a - 1.0) / (a + 1.0); }
    } else if (a < 2.4375) { id = 2; a = (a - 1.5) / (1.0 + 1.5 * a); }
    else { id = 3; a = -1.0 / a; }
    z = a * a;
    w = z * z;
    s1 = z * (aT[0] + w * (aT[2] + w * (aT[4] + w * (aT[6] + w * (aT[8] + w * aT[10])))));
    s2 = w * (aT[1] + w * (aT[3] + w * (aT[5] + w * (aT[7] + w * aT[9]))));
    if (id < 0) return copysign(a - a * (s1 + s2), x);
    z = atanhi[id] - ((a * (s1 + s2) - atanlo[id]) - a);
    return copysign(z, x);
}

static double PI = 3.1415926535897931160E+00;
static double PI_LO = 1.2246467991473531772E-16;

double atan2(double y, double x) {
    double z;
    if (__c4ix_isnan(x) || __c4ix_isnan(y)) return x + y;
    if (x == 1.0) return atan(y);
    if (y == 0) {
        if (m_neg(x)) return copysign(PI, y);
        return y;                                   // +-0
    }
    if (x == 0) return copysign(PI / 2, y);
    if (__c4ix_isinf(x)) {
        if (__c4ix_isinf(y)) return copysign(m_neg(x) ? 3 * PI / 4 : PI / 4, y);
        return m_neg(x) ? copysign(PI, y) : copysign(0.0, y);
    }
    if (__c4ix_isinf(y)) return copysign(PI / 2, y);
    z = atan(fabs(y / x));
    if (m_neg(x)) z = PI - (z - PI_LO);
    return copysign(z, y);
}
double asin(double x) {
    if (fabs(x) > 1) { errno = 33; return (x - x) / (x - x); }
    if (fabs(x) == 1) return copysign(PI / 2, x);
    return atan(x / sqrt((1 - x) * (1 + x)));
}
double acos(double x) {
    if (fabs(x) > 1) { errno = 33; return (x - x) / (x - x); }
    if (x == 1) return 0.0;
    if (x == -1) return PI;
    return 2 * atan(sqrt((1 - x) / (1 + x)));
}

// ---- hyperbolic ----

double sinh(double x) {
    double e;
    if (fabs(x) < 1e-5) return x + x * x * x / 6;
    if (fabs(x) < 1) { e = expm1(fabs(x)); return copysign(0.5 * (e + e / (e + 1)), x); }
    e = exp(fabs(x));
    return copysign(0.5 * (e - 1 / e), x);
}
double cosh(double x) {
    double e;
    e = exp(fabs(x));
    return 0.5 * (e + 1 / e);
}
double tanh(double x) {
    double e;
    if (fabs(x) > 22) return copysign(1.0, x);
    if (fabs(x) < 1e-5) return x - x * x * x / 3;
    e = expm1(2 * fabs(x));
    return copysign(e / (e + 2), x);
}

// ---- float versions: the double answer, rounded ----

float sqrtf(float x) { return (float)sqrt(x); }
float fabsf(float x) { return (float)fabs(x); }
float floorf(float x) { return (float)floor(x); }
float ceilf(float x) { return (float)ceil(x); }
float truncf(float x) { return (float)trunc(x); }
float roundf(float x) { return (float)round(x); }
float fmodf(float x, float y) { return (float)fmod(x, y); }
float expf(float x) { return (float)exp(x); }
float logf(float x) { return (float)log(x); }
float log10f(float x) { return (float)log10(x); }
float powf(float x, float y) { return (float)pow(x, y); }
float sinf(float x) { return (float)sin(x); }
float cosf(float x) { return (float)cos(x); }
float tanf(float x) { return (float)tan(x); }
float atanf(float x) { return (float)atan(x); }
float atan2f(float y, float x) { return (float)atan2(y, x); }
float asinf(float x) { return (float)asin(x); }
float acosf(float x) { return (float)acos(x); }
float hypotf(float x, float y) { return (float)hypot(x, y); }

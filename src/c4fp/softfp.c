// softfp.c -- IEEE 754 binary32 and binary64 arithmetic in software.
//
// What c4lc calls for `float` and `double` (docs/c4-float.md): every
// operation on them is a call to one of the __c4f_* (binary32) or
// __c4d_* (binary64) functions here. A float is its bit pattern in an
// `unsigned int`; a double is its bit pattern in an `unsigned long long`
// -- one word on a 64-bit machine, two on a 32-bit one, where c4lc
// already carries long long the same way. So this is ordinary C, and the
// same source serves both word sizes, c4lc and gcc alike.
//
// The arithmetic is IEEE's: correctly rounded, to nearest with ties to
// even (the only mode C programs here can ask for), with subnormals,
// signed zeros, infinities and NaNs. The algorithms are Berkeley
// SoftFloat 3's (John Hauser), rewritten -- add, subtract, multiply and
// the conversions follow it closely; divide and square root are exact
// digit-at-a-time loops instead of its reciprocal tables, slower and much
// smaller. NaNs behave as x86's SSE does, so results can be checked bit
// for bit against the host: an operation on a NaN returns its first NaN
// operand, quieted; an invalid one (inf - inf, 0 * inf, 0 / 0, sqrt of a
// negative) returns the "default NaN", negative and quiet. A conversion
// to an integer that cannot be represented gives the most negative value
// (x86's "integer indefinite") or, unsigned, all ones.
//
// No exception flags: nothing in C4 reads them.
//
// No typedefs and no globals, so this file can be spliced into any unit
// (c4lc does that) without stepping on its names.

// ---- small pieces ------------------------------------------------------------

int __c4fp_clz32(unsigned int a) {
    int n;
    if (!a) return 32;
    n = 0;
    if (!(a & 0xFFFF0000)) { n = n + 16; a = a << 16; }
    if (!(a & 0xFF000000)) { n = n + 8; a = a << 8; }
    if (!(a & 0xF0000000)) { n = n + 4; a = a << 4; }
    if (!(a & 0xC0000000)) { n = n + 2; a = a << 2; }
    if (!(a & 0x80000000)) n = n + 1;
    return n;
}

int __c4fp_clz64(unsigned long long a) {
    unsigned int hi;
    hi = (unsigned int)(a >> 32);
    if (hi) return __c4fp_clz32(hi);
    return 32 + __c4fp_clz32((unsigned int)a);
}

// a >> dist, with any bit shifted out ORed into the lowest ("jamming")
unsigned int __c4fp_jam32(unsigned int a, int dist) {
    if (dist <= 0) return a;
    if (dist < 31) return (a >> dist) | ((a << (32 - dist)) != 0);
    return a != 0;
}

unsigned long long __c4fp_jam64(unsigned long long a, int dist) {
    if (dist <= 0) return a;
    if (dist < 63) return (a >> dist) | ((a << (64 - dist)) != 0);
    return a != 0;
}

// ---- binary32 --------------------------------------------------------------
//
// The working form is SoftFloat's: a significand with its leading 1 at
// bit 30 (7 bits below the 24 kept, for rounding), and an exponent one
// less than the biased exponent of the result -- so packing ADDS the
// significand, and a rounding that carries into bit 24 bumps the
// exponent by itself.

unsigned int __c4f_pack(unsigned int sign, int exp, unsigned int sig) {
    return (sign << 31) + ((unsigned int)exp << 23) + sig;
}

unsigned int __c4f_round(unsigned int sign, int exp, unsigned int sig) {
    unsigned int rbits;
    rbits = sig & 0x7F;
    if ((unsigned int)exp >= 0xFD) {
        if (exp < 0) {
            sig = __c4fp_jam32(sig, 0 - exp);
            exp = 0;
            rbits = sig & 0x7F;
        } else if (exp > 0xFD || sig + 0x40 >= 0x80000000) {
            return __c4f_pack(sign, 0xFF, 0);          // overflow: infinity
        }
    }
    sig = (sig + 0x40) >> 7;
    if (rbits == 0x40) sig = sig & ~1;                  // a tie: to even
    if (!sig) exp = 0;
    return __c4f_pack(sign, exp, sig);
}

unsigned int __c4f_normround(unsigned int sign, int exp, unsigned int sig) {
    int sh;
    sh = __c4fp_clz32(sig) - 1;
    exp = exp - sh;
    if (sh >= 7 && (unsigned int)exp < 0xFD)
        return __c4f_pack(sign, sig ? exp : 0, sig << (sh - 7));
    return __c4f_round(sign, exp, sig << sh);
}

int __c4f_isnan(unsigned int a) { return (a & 0x7FFFFFFF) > 0x7F800000; }

// the first NaN of the two, quieted
unsigned int __c4f_nan2(unsigned int a, unsigned int b) {
    if (__c4f_isnan(a)) return a | 0x00400000;
    return b | 0x00400000;
}

unsigned int __c4f_add_mags(unsigned int a, unsigned int b, unsigned int sign) {
    int ea, eb, ez, d;
    unsigned int sa, sb, sz;
    ea = (a >> 23) & 0xFF; sa = a & 0x007FFFFF;
    eb = (b >> 23) & 0xFF; sb = b & 0x007FFFFF;
    d = ea - eb;
    if (!d) {
        if (!ea) return a + sb;                          // two subnormals: just add
        if (ea == 0xFF) { if (sa | sb) return __c4f_nan2(a, b); return a; }
        ez = ea;
        sz = 0x01000000 + sa + sb;
        if (!(sz & 1) && ez < 0xFE) return __c4f_pack(sign, ez, sz >> 1);
        sz = sz << 6;
    } else {
        sa = sa << 6; sb = sb << 6;
        if (d < 0) {
            if (eb == 0xFF) { if (sb) return __c4f_nan2(a, b); return __c4f_pack(sign, 0xFF, 0); }
            ez = eb;
            sa = sa + (ea ? 0x20000000 : sa);
            sa = __c4fp_jam32(sa, 0 - d);
        } else {
            if (ea == 0xFF) { if (sa) return __c4f_nan2(a, b); return a; }
            ez = ea;
            sb = sb + (eb ? 0x20000000 : sb);
            sb = __c4fp_jam32(sb, d);
        }
        sz = 0x20000000 + sa + sb;
        if (sz < 0x40000000) { ez = ez - 1; sz = sz << 1; }
    }
    return __c4f_round(sign, ez, sz);
}

unsigned int __c4f_sub_mags(unsigned int a, unsigned int b, unsigned int sign) {
    int ea, eb, ez, d, sh;
    unsigned int sa, sb, sx, sy;
    int diff;
    ea = (a >> 23) & 0xFF; sa = a & 0x007FFFFF;
    eb = (b >> 23) & 0xFF; sb = b & 0x007FFFFF;
    d = ea - eb;
    if (!d) {
        if (ea == 0xFF) { if (sa | sb) return __c4f_nan2(a, b); return 0xFFC00000; }
        diff = (int)sa - (int)sb;
        if (!diff) return 0;                             // x - x is +0
        if (ea) ea = ea - 1;
        if (diff < 0) { sign = sign ^ 1; diff = 0 - diff; }
        sh = __c4fp_clz32((unsigned int)diff) - 8;
        ez = ea - sh;
        if (ez < 0) { sh = ea; ez = 0; }
        return __c4f_pack(sign, ez, (unsigned int)diff << sh);
    }
    sa = sa << 7; sb = sb << 7;
    if (d < 0) {
        sign = sign ^ 1;
        if (eb == 0xFF) { if (sb) return __c4f_nan2(a, b); return __c4f_pack(sign, 0xFF, 0); }
        ez = eb - 1;
        sx = sb | 0x40000000;
        sy = sa + (ea ? 0x40000000 : sa);
        d = 0 - d;
    } else {
        if (ea == 0xFF) { if (sa) return __c4f_nan2(a, b); return a; }
        ez = ea - 1;
        sx = sa | 0x40000000;
        sy = sb + (eb ? 0x40000000 : sb);
    }
    return __c4f_normround(sign, ez, sx - __c4fp_jam32(sy, d));
}

unsigned int __c4f_add(unsigned int a, unsigned int b) {
    if ((a ^ b) >> 31) return __c4f_sub_mags(a, b, a >> 31);
    return __c4f_add_mags(a, b, a >> 31);
}

unsigned int __c4f_sub(unsigned int a, unsigned int b) {
    if ((a ^ b) >> 31) return __c4f_add_mags(a, b, a >> 31);
    return __c4f_sub_mags(a, b, a >> 31);
}

// a subnormal's significand normalised: the exponent it then has
int __c4f_normsub(unsigned int *sig) {
    int sh;
    sh = __c4fp_clz32(*sig) - 8;
    *sig = *sig << sh;
    return 1 - sh;
}

unsigned int __c4f_mul(unsigned int a, unsigned int b) {
    int ea, eb, ez;
    unsigned int sa, sb, sz, sign, mag;
    unsigned long long p;
    sign = (a ^ b) >> 31;
    ea = (a >> 23) & 0xFF; sa = a & 0x007FFFFF;
    eb = (b >> 23) & 0xFF; sb = b & 0x007FFFFF;
    if (ea == 0xFF) {
        if (sa || (eb == 0xFF && sb)) return __c4f_nan2(a, b);
        mag = eb | sb;
        if (!mag) return 0xFFC00000;                     // inf * 0
        return __c4f_pack(sign, 0xFF, 0);
    }
    if (eb == 0xFF) {
        if (sb) return __c4f_nan2(a, b);
        mag = ea | sa;
        if (!mag) return 0xFFC00000;
        return __c4f_pack(sign, 0xFF, 0);
    }
    if (!ea) { if (!sa) return __c4f_pack(sign, 0, 0); ea = __c4f_normsub(&sa); }
    if (!eb) { if (!sb) return __c4f_pack(sign, 0, 0); eb = __c4f_normsub(&sb); }
    ez = ea + eb - 0x7F;
    sa = (sa | 0x00800000) << 7;
    sb = (sb | 0x00800000) << 8;
    p = (unsigned long long)sa * sb;
    sz = (unsigned int)(p >> 32) | ((unsigned int)p != 0);
    if (sz < 0x40000000) { ez = ez - 1; sz = sz << 1; }
    return __c4f_round(sign, ez, sz);
}

unsigned int __c4f_div(unsigned int a, unsigned int b) {
    int ea, eb, ez, i;
    unsigned int sa, sb, q, sign;
    sign = (a ^ b) >> 31;
    ea = (a >> 23) & 0xFF; sa = a & 0x007FFFFF;
    eb = (b >> 23) & 0xFF; sb = b & 0x007FFFFF;
    if (ea == 0xFF) {
        if (sa) return __c4f_nan2(a, b);
        if (eb == 0xFF) { if (sb) return __c4f_nan2(a, b); return 0xFFC00000; }
        return __c4f_pack(sign, 0xFF, 0);
    }
    if (eb == 0xFF) { if (sb) return __c4f_nan2(a, b); return __c4f_pack(sign, 0, 0); }
    if (!eb) {
        if (!sb) {
            if (!(ea | sa)) return 0xFFC00000;           // 0 / 0
            return __c4f_pack(sign, 0xFF, 0);            // x / 0
        }
        eb = __c4f_normsub(&sb);
    }
    if (!ea) { if (!sa) return __c4f_pack(sign, 0, 0); ea = __c4f_normsub(&sa); }
    ez = ea - eb + 0x7E;
    sa = sa | 0x00800000;
    sb = sb | 0x00800000;
    if (sa < sb) { ez = ez - 1; sa = sa << 1; }
    // 31 quotient bits of sa / sb (1 <= sa/sb < 2), one at a time; the
    // remainder stays under 2 * sb < 2^25, so a word holds it
    q = 0;
    i = 0;
    while (i < 31) {
        q = q << 1;
        if (sa >= sb) { sa = sa - sb; q = q | 1; }
        sa = sa << 1;
        ++i;
    }
    return __c4f_round(sign, ez, q | (sa != 0));
}

// The root of m * 2^e (m in [1, 2), e made even): digit by digit, two
// bits of the radicand in per bit of root out, the remainder kept. 26
// root bits are the 24 kept plus two, and a nonzero remainder is the
// sticky bit -- all a correct rounding needs -- and every quantity
// stays under 2^29, a word at either size.
unsigned int __c4f_sqrt(unsigned int a) {
    int ea, e, i;
    unsigned int sa, rem, root, trial;
    unsigned long long r;
    ea = (a >> 23) & 0xFF; sa = a & 0x007FFFFF;
    if (ea == 0xFF) {
        if (sa) return a | 0x00400000;
        if (!(a >> 31)) return a;
        return 0xFFC00000;
    }
    if (a >> 31) {
        if (!(ea | sa)) return a;                        // sqrt(-0) is -0
        return 0xFFC00000;
    }
    if (!ea) { if (!sa) return a; ea = __c4f_normsub(&sa); }
    e = ea - 0x7F;
    sa = sa | 0x00800000;
    // the radicand: m * 2^50, in [2^50, 2^52); its root is in [2^25, 2^26)
    if (e & 1) { r = (unsigned long long)sa << 28; e = e - 1; }
    else r = (unsigned long long)sa << 27;
    rem = 0; root = 0;
    i = 25;
    while (i >= 0) {
        rem = (rem << 2) | ((unsigned int)(r >> (2 * i)) & 3);
        trial = (root << 2) | 1;
        root = root << 1;
        if (rem >= trial) { rem = rem - trial; root = root | 1; }
        --i;
    }
    return __c4f_round(0, e / 2 + 0x7E, (root << 5) | (rem != 0));
}

// -1 a < b, 0 equal, 1 a > b, 2 unordered (a NaN)
int __c4f_cmp(unsigned int a, unsigned int b) {
    if (__c4f_isnan(a) || __c4f_isnan(b)) return 2;
    if (!((a | b) & 0x7FFFFFFF)) return 0;               // +0 == -0
    if (a == b) return 0;
    if ((a ^ b) >> 31) return (a >> 31) ? 0 - 1 : 1;
    if ((a < b) ^ (a >> 31)) return 0 - 1;
    return 1;
}

unsigned int __c4f_neg(unsigned int a) { return a ^ 0x80000000; }
unsigned int __c4f_abs(unsigned int a) { return a & 0x7FFFFFFF; }

unsigned int __c4f_from_ull_s(unsigned long long a, unsigned int sign) {
    int sh;
    unsigned int sig;
    sh = __c4fp_clz64(a) - 40;
    if (sh >= 0) {
        if (!a) return __c4f_pack(sign, 0, 0);
        return __c4f_pack(sign, 0x95 - sh, (unsigned int)a << sh);
    }
    sh = sh + 7;
    if (sh < 0) sig = (unsigned int)__c4fp_jam64(a, 0 - sh);
    else sig = (unsigned int)a << sh;
    return __c4f_round(sign, 0x9C - sh, sig);
}

unsigned int __c4f_from_ull(unsigned long long a) { return __c4f_from_ull_s(a, 0); }
unsigned int __c4f_from_ll(long long a) {
    if (a < 0) return __c4f_from_ull_s(0 - (unsigned long long)a, 1);
    return __c4f_from_ull_s((unsigned long long)a, 0);
}
unsigned int __c4f_from_i(int a) { return __c4f_from_ll((long long)a); }
unsigned int __c4f_from_u(unsigned int a) { return __c4f_from_ull_s((unsigned long long)a, 0); }

// toward zero; what does not fit is "integer indefinite", as on x86
long long __c4f_to_ll(unsigned int a) {
    int e, sh;
    unsigned int sig;
    unsigned long long m;
    e = (a >> 23) & 0xFF; sig = a & 0x007FFFFF;
    sh = 0xBE - e;
    if (sh <= 0) return (long long)0x8000000000000000ull;   // |a| >= 2^63, inf, NaN
    if (sh >= 64) return 0;
    m = (unsigned long long)(sig | 0x00800000) << 40;
    m = m >> sh;
    if (a >> 31) return 0 - (long long)m;
    return (long long)m;
}

unsigned long long __c4f_to_ull(unsigned int a) {
    int e, sh;
    unsigned int sig;
    unsigned long long m;
    e = (a >> 23) & 0xFF; sig = a & 0x007FFFFF;
    sh = 0xBE - e;
    if (sh >= 64 || !(e | sig)) return 0;                // |a| < 1
    if ((a >> 31) || sh < 0) return 0xFFFFFFFFFFFFFFFFull;
    m = (unsigned long long)(sig | 0x00800000) << 40;
    return m >> sh;
}

int __c4f_to_i(unsigned int a) {
    long long v;
    v = __c4f_to_ll(a);
    if (sizeof(int) == 4 && (v < 0 - 2147483647 - 1 || v > 2147483647)) return 0 - 2147483647 - 1;
    return (int)v;
}

unsigned int __c4f_to_u(unsigned int a) {
    unsigned long long v;
    v = __c4f_to_ull(a);
    if (v > 0xFFFFFFFF) return 0xFFFFFFFF;
    return (unsigned int)v;
}

// ---- binary64 --------------------------------------------------------------
//
// The same scheme a word wider: the leading 1 at bit 62, 10 bits below
// the 53 kept.

unsigned long long __c4d_pack(unsigned int sign, int exp, unsigned long long sig) {
    return ((unsigned long long)sign << 63) + ((unsigned long long)(unsigned int)exp << 52) + sig;
}

unsigned long long __c4d_round(unsigned int sign, int exp, unsigned long long sig) {
    unsigned int rbits;
    rbits = (unsigned int)sig & 0x3FF;
    if ((unsigned int)exp >= 0x7FD) {
        if (exp < 0) {
            sig = __c4fp_jam64(sig, 0 - exp);
            exp = 0;
            rbits = (unsigned int)sig & 0x3FF;
        } else if (exp > 0x7FD || sig + 0x200 >= 0x8000000000000000ull) {
            return __c4d_pack(sign, 0x7FF, 0);
        }
    }
    sig = (sig + 0x200) >> 10;
    if (rbits == 0x200) sig = sig & ~(unsigned long long)1;
    if (!sig) exp = 0;
    return __c4d_pack(sign, exp, sig);
}

unsigned long long __c4d_normround(unsigned int sign, int exp, unsigned long long sig) {
    int sh;
    sh = __c4fp_clz64(sig) - 1;
    exp = exp - sh;
    if (sh >= 10 && (unsigned int)exp < 0x7FD)
        return __c4d_pack(sign, sig ? exp : 0, sig << (sh - 10));
    return __c4d_round(sign, exp, sig << sh);
}

int __c4d_isnan(unsigned long long a) { return (a & 0x7FFFFFFFFFFFFFFFull) > 0x7FF0000000000000ull; }

unsigned long long __c4d_nan2(unsigned long long a, unsigned long long b) {
    if (__c4d_isnan(a)) return a | 0x0008000000000000ull;
    return b | 0x0008000000000000ull;
}

unsigned long long __c4d_add_mags(unsigned long long a, unsigned long long b, unsigned int sign) {
    int ea, eb, ez, d;
    unsigned long long sa, sb, sz;
    ea = (int)(a >> 52) & 0x7FF; sa = a & 0x000FFFFFFFFFFFFFull;
    eb = (int)(b >> 52) & 0x7FF; sb = b & 0x000FFFFFFFFFFFFFull;
    d = ea - eb;
    if (!d) {
        if (!ea) return a + sb;
        if (ea == 0x7FF) { if (sa | sb) return __c4d_nan2(a, b); return a; }
        ez = ea;
        sz = 0x0020000000000000ull + sa + sb;
        sz = sz << 9;
    } else {
        sa = sa << 9; sb = sb << 9;
        if (d < 0) {
            if (eb == 0x7FF) { if (sb) return __c4d_nan2(a, b); return __c4d_pack(sign, 0x7FF, 0); }
            ez = eb;
            if (ea) sa = sa + 0x2000000000000000ull; else sa = sa << 1;
            sa = __c4fp_jam64(sa, 0 - d);
        } else {
            if (ea == 0x7FF) { if (sa) return __c4d_nan2(a, b); return a; }
            ez = ea;
            if (eb) sb = sb + 0x2000000000000000ull; else sb = sb << 1;
            sb = __c4fp_jam64(sb, d);
        }
        sz = 0x2000000000000000ull + sa + sb;
        if (sz < 0x4000000000000000ull) { ez = ez - 1; sz = sz << 1; }
    }
    return __c4d_round(sign, ez, sz);
}

unsigned long long __c4d_sub_mags(unsigned long long a, unsigned long long b, unsigned int sign) {
    int ea, eb, ez, d, sh;
    unsigned long long sa, sb, sz, diff;
    ea = (int)(a >> 52) & 0x7FF; sa = a & 0x000FFFFFFFFFFFFFull;
    eb = (int)(b >> 52) & 0x7FF; sb = b & 0x000FFFFFFFFFFFFFull;
    d = ea - eb;
    if (!d) {
        if (ea == 0x7FF) { if (sa | sb) return __c4d_nan2(a, b); return 0xFFF8000000000000ull; }
        if (sa == sb) return 0;
        if (ea) ea = ea - 1;
        if (sa < sb) { sign = sign ^ 1; diff = sb - sa; } else diff = sa - sb;
        sh = __c4fp_clz64(diff) - 11;
        ez = ea - sh;
        if (ez < 0) { sh = ea; ez = 0; }
        return __c4d_pack(sign, ez, diff << sh);
    }
    sa = sa << 10; sb = sb << 10;
    if (d < 0) {
        sign = sign ^ 1;
        if (eb == 0x7FF) { if (sb) return __c4d_nan2(a, b); return __c4d_pack(sign, 0x7FF, 0); }
        sa = sa + (ea ? 0x4000000000000000ull : sa);
        sa = __c4fp_jam64(sa, 0 - d);
        sb = sb | 0x4000000000000000ull;
        ez = eb;
        sz = sb - sa;
    } else {
        if (ea == 0x7FF) { if (sa) return __c4d_nan2(a, b); return a; }
        sb = sb + (eb ? 0x4000000000000000ull : sb);
        sb = __c4fp_jam64(sb, d);
        sa = sa | 0x4000000000000000ull;
        ez = ea;
        sz = sa - sb;
    }
    return __c4d_normround(sign, ez - 1, sz);
}

unsigned long long __c4d_add(unsigned long long a, unsigned long long b) {
    unsigned int sa;
    sa = (unsigned int)(a >> 63);
    if (sa != (unsigned int)(b >> 63)) return __c4d_sub_mags(a, b, sa);
    return __c4d_add_mags(a, b, sa);
}

unsigned long long __c4d_sub(unsigned long long a, unsigned long long b) {
    unsigned int sa;
    sa = (unsigned int)(a >> 63);
    if (sa != (unsigned int)(b >> 63)) return __c4d_add_mags(a, b, sa);
    return __c4d_sub_mags(a, b, sa);
}

int __c4d_normsub(unsigned long long *sig) {
    int sh;
    sh = __c4fp_clz64(*sig) - 11;
    *sig = *sig << sh;
    return 1 - sh;
}

unsigned long long __c4d_mul(unsigned long long a, unsigned long long b) {
    int ea, eb, ez;
    unsigned int sign, a0, a1, b0, b1;
    unsigned long long sa, sb, sz, mag, p00, p01, p10, p11, mid, hi, lo;
    sign = (unsigned int)((a ^ b) >> 63);
    ea = (int)(a >> 52) & 0x7FF; sa = a & 0x000FFFFFFFFFFFFFull;
    eb = (int)(b >> 52) & 0x7FF; sb = b & 0x000FFFFFFFFFFFFFull;
    if (ea == 0x7FF) {
        if (sa || (eb == 0x7FF && sb)) return __c4d_nan2(a, b);
        mag = (unsigned long long)eb | sb;
        if (!mag) return 0xFFF8000000000000ull;
        return __c4d_pack(sign, 0x7FF, 0);
    }
    if (eb == 0x7FF) {
        if (sb) return __c4d_nan2(a, b);
        mag = (unsigned long long)ea | sa;
        if (!mag) return 0xFFF8000000000000ull;
        return __c4d_pack(sign, 0x7FF, 0);
    }
    if (!ea) { if (!sa) return __c4d_pack(sign, 0, 0); ea = __c4d_normsub(&sa); }
    if (!eb) { if (!sb) return __c4d_pack(sign, 0, 0); eb = __c4d_normsub(&sb); }
    ez = ea + eb - 0x3FF;
    sa = (sa | 0x0010000000000000ull) << 10;
    sb = (sb | 0x0010000000000000ull) << 11;
    // the 128-bit product, from 32-bit halves
    a0 = (unsigned int)sa; a1 = (unsigned int)(sa >> 32);
    b0 = (unsigned int)sb; b1 = (unsigned int)(sb >> 32);
    p00 = (unsigned long long)a0 * b0;
    p01 = (unsigned long long)a0 * b1;
    p10 = (unsigned long long)a1 * b0;
    p11 = (unsigned long long)a1 * b1;
    mid = (p00 >> 32) + (unsigned int)p01 + (unsigned int)p10;
    lo = (mid << 32) | (unsigned int)p00;
    hi = p11 + (p01 >> 32) + (p10 >> 32) + (mid >> 32);
    sz = hi | (lo != 0);
    if (sz < 0x4000000000000000ull) { ez = ez - 1; sz = sz << 1; }
    return __c4d_round(sign, ez, sz);
}

unsigned long long __c4d_div(unsigned long long a, unsigned long long b) {
    int ea, eb, ez, i;
    unsigned int sign;
    unsigned long long sa, sb, q;
    sign = (unsigned int)((a ^ b) >> 63);
    ea = (int)(a >> 52) & 0x7FF; sa = a & 0x000FFFFFFFFFFFFFull;
    eb = (int)(b >> 52) & 0x7FF; sb = b & 0x000FFFFFFFFFFFFFull;
    if (ea == 0x7FF) {
        if (sa) return __c4d_nan2(a, b);
        if (eb == 0x7FF) { if (sb) return __c4d_nan2(a, b); return 0xFFF8000000000000ull; }
        return __c4d_pack(sign, 0x7FF, 0);
    }
    if (eb == 0x7FF) { if (sb) return __c4d_nan2(a, b); return __c4d_pack(sign, 0, 0); }
    if (!eb) {
        if (!sb) {
            if (!((unsigned long long)ea | sa)) return 0xFFF8000000000000ull;
            return __c4d_pack(sign, 0x7FF, 0);
        }
        eb = __c4d_normsub(&sb);
    }
    if (!ea) { if (!sa) return __c4d_pack(sign, 0, 0); ea = __c4d_normsub(&sa); }
    ez = ea - eb + 0x3FE;
    sa = sa | 0x0010000000000000ull;
    sb = sb | 0x0010000000000000ull;
    if (sa < sb) { ez = ez - 1; sa = sa << 1; }
    q = 0;
    i = 0;
    while (i < 63) {
        q = q << 1;
        if (sa >= sb) { sa = sa - sb; q = q | 1; }
        sa = sa << 1;
        ++i;
    }
    return __c4d_round(sign, ez, q | (sa != 0));
}

// as __c4f_sqrt: 55 root bits (53 + 2) from the radicand m * 2^108, in
// [2^108, 2^110), held as two halves; the remainder stays under 2^58
unsigned long long __c4d_sqrt(unsigned long long a) {
    int ea, e, i, pos;
    unsigned long long sa, rhi, rlo, rem, root, trial, pair;
    ea = (int)(a >> 52) & 0x7FF; sa = a & 0x000FFFFFFFFFFFFFull;
    if (ea == 0x7FF) {
        if (sa) return a | 0x0008000000000000ull;
        if (!(a >> 63)) return a;
        return 0xFFF8000000000000ull;
    }
    if (a >> 63) {
        if (!((unsigned long long)ea | sa)) return a;
        return 0xFFF8000000000000ull;
    }
    if (!ea) { if (!sa) return a; ea = __c4d_normsub(&sa); }
    e = ea - 0x3FF;
    sa = sa | 0x0010000000000000ull;
    if (e & 1) { rhi = sa >> 7; rlo = sa << 57; e = e - 1; }
    else { rhi = sa >> 8; rlo = sa << 56; }
    rem = 0; root = 0;
    i = 54;
    while (i >= 0) {
        pos = 2 * i;                                     // even: a pair never straddles the halves
        if (pos >= 64) pair = (rhi >> (pos - 64)) & 3;
        else pair = (rlo >> pos) & 3;
        rem = (rem << 2) | pair;
        trial = (root << 2) | 1;
        root = root << 1;
        if (rem >= trial) { rem = rem - trial; root = root | 1; }
        --i;
    }
    return __c4d_round(0, e / 2 + 0x3FE, (root << 8) | (rem != 0));
}

int __c4d_cmp(unsigned long long a, unsigned long long b) {
    if (__c4d_isnan(a) || __c4d_isnan(b)) return 2;
    if (!((a | b) & 0x7FFFFFFFFFFFFFFFull)) return 0;
    if (a == b) return 0;
    if ((a ^ b) >> 63) return (a >> 63) ? 0 - 1 : 1;
    if ((a < b) ^ (unsigned int)(a >> 63)) return 0 - 1;
    return 1;
}

unsigned long long __c4d_neg(unsigned long long a) { return a ^ 0x8000000000000000ull; }
unsigned long long __c4d_abs(unsigned long long a) { return a & 0x7FFFFFFFFFFFFFFFull; }

unsigned long long __c4d_from_ull_s(unsigned long long a, unsigned int sign) {
    if (!a) return __c4d_pack(sign, 0, 0);
    if (a >> 63) return __c4d_round(sign, 0x43D, __c4fp_jam64(a, 1));
    return __c4d_normround(sign, 0x43C, a);
}

unsigned long long __c4d_from_ull(unsigned long long a) { return __c4d_from_ull_s(a, 0); }
unsigned long long __c4d_from_ll(long long a) {
    if (a < 0) return __c4d_from_ull_s(0 - (unsigned long long)a, 1);
    return __c4d_from_ull_s((unsigned long long)a, 0);
}
unsigned long long __c4d_from_i(int a) { return __c4d_from_ll((long long)a); }
unsigned long long __c4d_from_u(unsigned int a) { return __c4d_from_ull_s((unsigned long long)a, 0); }

long long __c4d_to_ll(unsigned long long a) {
    int e, sh;
    unsigned long long m;
    e = (int)(a >> 52) & 0x7FF;
    m = (a & 0x000FFFFFFFFFFFFFull) | 0x0010000000000000ull;
    sh = 0x433 - e;
    if (sh <= 0) {
        if (sh < 0 - 10) return (long long)0x8000000000000000ull;   // |a| >= 2^63, inf, NaN
        m = m << (0 - sh);
    } else {
        if (sh >= 53) return 0;
        m = m >> sh;
    }
    if (a >> 63) return 0 - (long long)m;
    return (long long)m;
}

unsigned long long __c4d_to_ull(unsigned long long a) {
    int e, sh;
    unsigned long long m;
    e = (int)(a >> 52) & 0x7FF;
    m = (a & 0x000FFFFFFFFFFFFFull) | 0x0010000000000000ull;
    sh = 0x433 - e;
    if (sh >= 53) return 0;                              // |a| < 1
    if ((a >> 63) || sh < 0 - 11) return 0xFFFFFFFFFFFFFFFFull;
    if (sh <= 0) return m << (0 - sh);
    return m >> sh;
}

int __c4d_to_i(unsigned long long a) {
    long long v;
    v = __c4d_to_ll(a);
    if (sizeof(int) == 4 && (v < 0 - 2147483647 - 1 || v > 2147483647)) return 0 - 2147483647 - 1;
    return (int)v;
}

unsigned int __c4d_to_u(unsigned long long a) {
    unsigned long long v;
    v = __c4d_to_ull(a);
    if (v > 0xFFFFFFFF) return 0xFFFFFFFF;
    return (unsigned int)v;
}

// ---- between the two -------------------------------------------------------

unsigned long long __c4f_to_d(unsigned int a) {
    int e;
    unsigned int sign, sig;
    sign = a >> 31;
    e = (a >> 23) & 0xFF; sig = a & 0x007FFFFF;
    if (e == 0xFF) {
        if (sig) return __c4d_pack(sign, 0x7FF, ((unsigned long long)sig << 29) | 0x0008000000000000ull);
        return __c4d_pack(sign, 0x7FF, 0);
    }
    if (!e) {
        if (!sig) return __c4d_pack(sign, 0, 0);
        e = __c4f_normsub(&sig) - 1;                     // the leading 1 now carries into the exponent
    }
    return __c4d_pack(sign, e + 0x380, (unsigned long long)sig << 29);
}

unsigned int __c4d_to_f(unsigned long long a) {
    int e;
    unsigned int sign, f;
    unsigned long long sig;
    sign = (unsigned int)(a >> 63);
    e = (int)(a >> 52) & 0x7FF; sig = a & 0x000FFFFFFFFFFFFFull;
    if (e == 0x7FF) {
        if (sig) return __c4f_pack(sign, 0xFF, 0x00400000 | (unsigned int)(sig >> 29));
        return __c4f_pack(sign, 0xFF, 0);
    }
    f = (unsigned int)__c4fp_jam64(sig, 22);
    if (!(e | f)) return __c4f_pack(sign, 0, 0);
    return __c4f_round(sign, e - 0x381, f | 0x40000000);
}

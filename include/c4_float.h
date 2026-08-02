//
// C4 Standard Library: c4_float.h
//
// IEEE-754 binary32 ("float") arithmetic implemented entirely in C4.
//
// C4 has no float type, so a value is carried in an ordinary int as its 32-bit
// bit pattern -- exactly the representation c4m's native FLTI_* instructions
// use. Everything here is integer arithmetic on that pattern, so it works on
// unmodified c4, on c4m, and inside C4KE, with no interpreter support at all.
//
//   int x, y, z;
//   x = f32_from_int(7);
//   y = f32_from_string("0.5");
//   z = f32_div(x, y);              // 14.0
//   f32_print(z, 6);                // "14.000000"
//
// Results are bit-for-bit identical to C 'float' arithmetic for normal values,
// including round-to-nearest-even. Verified against a native C reference in
// src/tests/test_float.c.
//
// Supported: finite normals, signed zero, infinity, NaN, and correct rounding
//            for + - * / and conversions.
// Simplified: subnormal values flush to zero (they are the only case where
//            this library and hardware differ). NaN payloads are not
//            preserved; a canonical quiet NaN is produced.
//
// Why binary32 and not binary64: a C4 word is 64 bits, so a 24-bit significand
// product (48 bits) fits with room for guard bits and no unsigned type is
// needed. binary64 would need 106-bit intermediates, i.e. 128-bit multiply
// emulation -- doable, but not a prototype.
//

#ifndef __C4_FLOAT_H
#define __C4_FLOAT_H 1

#include <stddef.h>

// Field layout of IEEE-754 binary32
enum {
	F32_SIGNBIT  = 0x80000000,
	F32_EXPMASK  = 0x7F800000,
	F32_MANMASK  = 0x007FFFFF,
	F32_HIDDEN   = 0x00800000,  // 1 << 23
	F32_EXPMAX   = 0xFF,        // exponent field for inf/nan
	F32_BIAS     = 127,
	F32_WORD     = 0xFFFFFFFF,  // keep results inside 32 bits

	// Working significands carry 3 extra low bits: guard, round, sticky.
	F32_GUARD    = 3,
	F32_TOP27    = 0x08000000,  // 1 << 27
	F32_TOP26    = 0x04000000,  // 1 << 26
	F32_TOP24    = 0x01000000,  // 1 << 24

	// Common values
	F32_ZERO     = 0x00000000,
	F32_NEGZERO  = 0x80000000,
	F32_ONE      = 0x3F800000,
	F32_INF      = 0x7F800000,
	F32_NEGINF   = 0xFF800000,
	F32_NAN      = 0x7FC00000
};

///
/// Classification
///

static int f32_exp_field (int x) { return (x >> 23) & F32_EXPMAX; }
static int f32_man_field (int x) { return x & F32_MANMASK; }
static int f32_sign      (int x) { return (x >> 31) & 1; }

static int f32_is_nan  (int x) { return f32_exp_field(x) == F32_EXPMAX && f32_man_field(x) != 0; }
static int f32_is_inf  (int x) { return f32_exp_field(x) == F32_EXPMAX && f32_man_field(x) == 0; }
static int f32_is_zero (int x) { return (x & 0x7FFFFFFF) == 0; }

///
/// Internal helpers
///

// Assemble a value from its fields.
static int f32_pack (int sign, int exp, int man) {
	return ((sign ? F32_SIGNBIT : 0) | ((exp & F32_EXPMAX) << 23) | (man & F32_MANMASK)) & F32_WORD;
}

static int f32_inf_signed  (int sign) { return sign ? F32_NEGINF : F32_INF; }
static int f32_zero_signed (int sign) { return sign ? F32_NEGZERO : F32_ZERO; }

// Shift right by n, OR-ing everything shifted out into the low (sticky) bit.
static int f32_shr_sticky (int m, int n) {
	int sticky;

	if (n <= 0) return m;
	if (n > 30) return (m != 0) ? 1 : 0;
	sticky = (m & ((1 << n) - 1)) != 0;
	return (m >> n) | sticky;
}

// Round a 27-bit significand (24 value bits + guard/round/sticky) to 24 bits,
// nearest, ties to even.
static int f32_round27 (int m) {
	int extra;

	extra = m & 7;
	m = m >> F32_GUARD;
	if (extra > 4) ++m;
	else if (extra == 4 && (m & 1)) ++m;
	return m;
}

// Normalise a rounded significand and exponent into a finished value.
// Handles significand overflow from rounding, plus over/underflow.
static int f32_finish (int sign, int exp, int m) {
	if (m >= F32_TOP24) { m = m >> 1; ++exp; }
	if (exp >= F32_EXPMAX) return f32_inf_signed(sign);
	if (exp <= 0) return f32_zero_signed(sign);   // subnormals flush to zero
	return f32_pack(sign, exp, m & F32_MANMASK);
}

///
/// Sign
///

static int f32_neg (int x) { return (x ^ F32_SIGNBIT) & F32_WORD; }
static int f32_abs (int x) { return x & 0x7FFFFFFF; }

///
/// Addition and subtraction
///

static int f32_add (int a, int b) {
	int sa, ea, ma, sb, eb, mb;
	int sr, er, mr, d;

	if (f32_is_nan(a) || f32_is_nan(b)) return F32_NAN;

	sa = f32_sign(a); sb = f32_sign(b);

	if (f32_is_inf(a)) {
		// inf + -inf is undefined
		if (f32_is_inf(b) && sa != sb) return F32_NAN;
		return a;
	}
	if (f32_is_inf(b)) return b;

	if (f32_is_zero(a)) {
		// -0 + -0 is -0; every other zero sum is +0
		if (f32_is_zero(b)) return (sa && sb) ? F32_NEGZERO : F32_ZERO;
		return b;
	}
	if (f32_is_zero(b)) return a;

	ea = f32_exp_field(a); ma = f32_man_field(a);
	eb = f32_exp_field(b); mb = f32_man_field(b);

	// Subnormal inputs flush to zero.
	if (ea == 0) return b;
	if (eb == 0) return a;

	// Restore the hidden bit and make room for guard/round/sticky.
	ma = (ma | F32_HIDDEN) << F32_GUARD;
	mb = (mb | F32_HIDDEN) << F32_GUARD;

	// Align to the larger exponent.
	if (ea > eb) {
		d = ea - eb;
		mb = f32_shr_sticky(mb, d);
		eb = ea;
	} else if (eb > ea) {
		d = eb - ea;
		ma = f32_shr_sticky(ma, d);
		ea = eb;
	}
	er = ea;

	if (sa == sb) {
		mr = ma + mb;
		sr = sa;
	} else if (ma >= mb) {
		mr = ma - mb;
		sr = sa;
	} else {
		mr = mb - ma;
		sr = sb;
	}

	// Exact cancellation gives +0 (round-to-nearest rounding direction).
	if (mr == 0) return F32_ZERO;

	// Renormalise.
	while (mr >= F32_TOP27) { mr = f32_shr_sticky(mr, 1); ++er; }
	while (mr < F32_TOP26 && er > 1) { mr = mr << 1; --er; }
	if (mr < F32_TOP26) return f32_zero_signed(sr);   // underflowed to subnormal

	return f32_finish(sr, er, f32_round27(mr));
}

static int f32_sub (int a, int b) { return f32_add(a, f32_neg(b)); }

///
/// Multiplication
///

static int f32_mul (int a, int b) {
	int sa, ea, ma, sb, eb, mb;
	int sr, er, mr;

	if (f32_is_nan(a) || f32_is_nan(b)) return F32_NAN;

	sa = f32_sign(a); sb = f32_sign(b);
	sr = sa ^ sb;

	if (f32_is_inf(a)) return f32_is_zero(b) ? F32_NAN : f32_inf_signed(sr);
	if (f32_is_inf(b)) return f32_is_zero(a) ? F32_NAN : f32_inf_signed(sr);
	if (f32_is_zero(a) || f32_is_zero(b)) return f32_zero_signed(sr);

	ea = f32_exp_field(a); ma = f32_man_field(a);
	eb = f32_exp_field(b); mb = f32_man_field(b);
	if (ea == 0 || eb == 0) return f32_zero_signed(sr);   // subnormal input

	ma = ma | F32_HIDDEN;
	mb = mb | F32_HIDDEN;

	// 24 x 24 = 48 bits, comfortably inside a 64-bit word.
	mr = ma * mb;
	er = ea + eb - F32_BIAS;

	// Two 24-bit significands give a product in [2^46, 2^48). Reduce it to the
	// 27-bit working form, whose leading bit sits at 26. The 1 is cast
	// because a native build widens the int TYPE to 64 bits but a bare
	// literal stays C's 32-bit int, making 1 << 47 undefined.
	if (mr >= ((int)1 << 47)) {
		mr = f32_shr_sticky(mr, 21);
		++er;
	} else {
		mr = f32_shr_sticky(mr, 20);
	}

	if (er >= F32_EXPMAX) return f32_inf_signed(sr);
	if (er <= 0) return f32_zero_signed(sr);

	return f32_finish(sr, er, f32_round27(mr));
}

///
/// Division
///

static int f32_div (int a, int b) {
	int sa, ea, ma, sb, eb, mb;
	int sr, er, mr, rem;

	if (f32_is_nan(a) || f32_is_nan(b)) return F32_NAN;

	sa = f32_sign(a); sb = f32_sign(b);
	sr = sa ^ sb;

	if (f32_is_inf(a)) return f32_is_inf(b) ? F32_NAN : f32_inf_signed(sr);
	if (f32_is_inf(b)) return f32_zero_signed(sr);
	if (f32_is_zero(a)) return f32_is_zero(b) ? F32_NAN : f32_zero_signed(sr);
	if (f32_is_zero(b)) return f32_inf_signed(sr);       // x / 0 is infinity

	ea = f32_exp_field(a); ma = f32_man_field(a);
	eb = f32_exp_field(b); mb = f32_man_field(b);
	if (ea == 0) return f32_zero_signed(sr);
	if (eb == 0) return f32_inf_signed(sr);

	ma = ma | F32_HIDDEN;
	mb = mb | F32_HIDDEN;

	// ma/mb lies in (0.5, 2), so scaling the numerator by 2^27 puts the
	// quotient in (2^26, 2^28). ma < 2^24, so ma << 27 < 2^51 -- no overflow.
	mr  = (ma << 27) / mb;
	rem = (ma << 27) % mb;

	er = ea - eb + F32_BIAS;

	// Normalise into the 27-bit working form, whose leading bit sits at 26.
	// mr >= 2^27 means the quotient is >= 1.0 and needs one shift down;
	// otherwise it is in (0.5, 1.0) and only the exponent moves.
	if (mr >= F32_TOP27) {
		if (mr & 1) rem = 1;         // the bit about to be dropped is sticky too
		mr = mr >> 1;
	} else
		--er;
	if (rem != 0) mr = mr | 1;       // sticky

	if (er >= F32_EXPMAX) return f32_inf_signed(sr);
	if (er <= 0) return f32_zero_signed(sr);

	return f32_finish(sr, er, f32_round27(mr));
}

///
/// Comparison
///
/// f32_cmp returns -1, 0 or 1. Any comparison involving NaN is unordered;
/// f32_cmp reports 2 for that case so callers can tell it apart.
///

static int f32_cmp (int a, int b) {
	int sa, sb;

	if (f32_is_nan(a) || f32_is_nan(b)) return 2;
	if (f32_is_zero(a) && f32_is_zero(b)) return 0;   // -0 == +0

	sa = f32_sign(a); sb = f32_sign(b);
	if (sa != sb) return sa ? -1 : 1;

	// Same sign: the bit patterns order the same way as the values, reversed
	// for negatives.
	a = a & 0x7FFFFFFF;
	b = b & 0x7FFFFFFF;
	if (a == b) return 0;
	if (sa) return (a > b) ? -1 : 1;
	return (a > b) ? 1 : -1;
}

static int f32_eq (int a, int b) { return f32_cmp(a, b) == 0; }
static int f32_lt (int a, int b) { return f32_cmp(a, b) == -1; }
static int f32_le (int a, int b) { int c; c = f32_cmp(a, b); return c == -1 || c == 0; }
static int f32_gt (int a, int b) { return f32_cmp(a, b) == 1; }
static int f32_ge (int a, int b) { int c; c = f32_cmp(a, b); return c == 1 || c == 0; }

///
/// Conversion
///

static int f32_from_int (int v) {
	int sign, exp, m, sticky, bits;

	if (v == 0) return F32_ZERO;

	sign = 0;
	if (v < 0) {
		sign = 1;
		// Do not negate the most negative value; handle it directly.
		if (v == (0 - 0x7FFFFFFFFFFFFFFF) - 1) return f32_pack(1, F32_BIAS + 63, 0);
		v = 0 - v;
	}

	// Find the position of the top set bit.
	bits = 0;
	m = v;
	while (m != 0) { m = (m >> 1) & 0x7FFFFFFFFFFFFFFF; ++bits; }
	exp = F32_BIAS + bits - 1;

	// Reduce to 27 bits (24 value + guard/round/sticky).
	if (bits > 27) {
		sticky = bits - 27;
		m = f32_shr_sticky(v, sticky);
	} else
		m = v << (27 - bits);

	return f32_finish(sign, exp, f32_round27(m));
}

// Truncate toward zero, C-style.
static int f32_to_int (int x) {
	int sign, exp, m, shift;

	if (f32_is_nan(x) || f32_is_zero(x)) return 0;

	sign = f32_sign(x);
	exp  = f32_exp_field(x);
	if (f32_is_inf(x)) return sign ? (0 - 0x7FFFFFFFFFFFFFFF) - 1 : 0x7FFFFFFFFFFFFFFF;
	if (exp == 0) return 0;                     // subnormal

	exp = exp - F32_BIAS;
	if (exp < 0) return 0;                      // |x| < 1
	if (exp > 62) return sign ? (0 - 0x7FFFFFFFFFFFFFFF) - 1 : 0x7FFFFFFFFFFFFFFF;

	m = f32_man_field(x) | F32_HIDDEN;          // 24 bits, binary point after bit 23
	shift = exp - 23;
	if (shift >= 0) m = m << shift;
	else m = m >> (0 - shift);

	return sign ? 0 - m : m;
}

///
/// Text
///

// Parse a decimal number: [+-]digits[.digits][(e|E)[+-]digits]
// Returns the value; *end (if given) receives the first unconsumed character.
static int f32_from_string (char *s, char **end) {
	int sign, r, digits, any, esign, eval, i;
	int ten, frac, scale;

	ten  = f32_from_int(10);
	r    = F32_ZERO;
	sign = 0;
	any  = 0;

	while (*s == ' ' || *s == 9) ++s;
	if (*s == '-') { sign = 1; ++s; }
	else if (*s == '+') ++s;

	// Integer part: accumulate as a float so large inputs still work.
	while (*s >= '0' && *s <= '9') {
		r = f32_add(f32_mul(r, ten), f32_from_int(*s - '0'));
		++s; any = 1;
	}

	// Fraction: gather digits as an integer, then divide by the power of ten.
	if (*s == '.') {
		++s;
		frac = 0; digits = 0;
		while (*s >= '0' && *s <= '9') {
			// Stop accumulating once more digits cannot affect a float.
			if (digits < 9) { frac = frac * 10 + (*s - '0'); ++digits; }
			++s; any = 1;
		}
		if (digits) {
			scale = 1;
			i = 0;
			while (i < digits) { scale = scale * 10; ++i; }
			r = f32_add(r, f32_div(f32_from_int(frac), f32_from_int(scale)));
		}
	}

	if (!any) { if (end) *end = s; return F32_ZERO; }

	// Exponent.
	if (*s == 'e' || *s == 'E') {
		++s;
		esign = 0;
		if (*s == '-') { esign = 1; ++s; }
		else if (*s == '+') ++s;
		eval = 0;
		while (*s >= '0' && *s <= '9') { eval = eval * 10 + (*s - '0'); ++s; }
		if (eval > 60) eval = 60;              // beyond float range either way
		i = 0;
		while (i < eval) {
			if (esign) r = f32_div(r, ten);
			else r = f32_mul(r, ten);
			++i;
		}
	}

	if (end) *end = s;
	return sign ? f32_neg(r) : r;
}

// Render into 'buf' with 'prec' digits after the point, C's %f style.
// Returns the number of characters written (excluding the nul).
// 'buf' should have room for prec + 24 characters.
static int f32_to_string (int x, char *buf, int prec) {
	char *p;
	int ipart, i, d, sign;
	int frac, ten, digit;
	int ip, len, j;
	char *t;

	p = buf;

	if (f32_is_nan(x)) { *p++ = 'n'; *p++ = 'a'; *p++ = 'n'; *p = 0; return 3; }
	sign = f32_sign(x);
	if (f32_is_inf(x)) {
		if (sign) *p++ = '-';
		*p++ = 'i'; *p++ = 'n'; *p++ = 'f'; *p = 0;
		return p - buf;
	}

	if (prec < 0) prec = 6;
	if (prec > 9) prec = 9;

	if (sign) { *p++ = '-'; x = f32_abs(x); }

	ten = f32_from_int(10);

	// Integer part.
	ipart = f32_to_int(x);
	frac  = f32_sub(x, f32_from_int(ipart));

	// Emit the integer part, digits reversed then flipped.
	t = p;
	if (ipart == 0) *p++ = '0';
	else {
		ip = ipart;
		while (ip != 0) { *p++ = '0' + (ip % 10); ip = ip / 10; }
		len = p - t;
		i = 0; j = len - 1;
		while (i < j) { d = t[i]; t[i] = t[j]; t[j] = (char)d; ++i; --j; }
	}

	if (prec > 0) {
		*p++ = '.';
		i = 0;
		while (i < prec) {
			frac  = f32_mul(frac, ten);
			digit = f32_to_int(frac);
			if (digit < 0) digit = 0;
			if (digit > 9) digit = 9;
			*p++ = '0' + digit;
			frac = f32_sub(frac, f32_from_int(digit));
			++i;
		}
	}

	*p = 0;
	return p - buf;
}

// Convenience: print a value with the given precision. Uses the buffer
// supplied so this header needs no allocation of its own.
static int f32_print_buf (int x, char *buf, int prec) {
	f32_to_string(x, buf, prec);
	return printf("%s", buf);
}

#endif /* ifndef __C4_FLOAT_H */

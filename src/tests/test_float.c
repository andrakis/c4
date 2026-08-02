// C4 Test: IEEE-754 binary32 arithmetic implemented in pure C4.
//
// The expected values in float_cases.h are produced by genfloat.c using real
// C 'float' arithmetic, so this checks the C4 implementation against hardware
// bit-for-bit rather than against its own assumptions.
//
// Build and run:
//   gcc -E -P -Iinclude -Isrc/tests -I. -DC4CC=1 -D__c4__=1 -D__c4cc__=1 \
//       src/tests/test_float.c | ./c4cc -o test_float.c4r -
//   ./c4m load-c4r.c -- test_float.c4r
//
// Also runs under plain c4 (only original-C4 opcodes are emitted) and as a
// C4KE process.

#include <stdio.h>
#include <stdlib.h>
#include <c4_float.h>

#include "float_cases.h"

static int total, fails;

// Parse hex digits until a non-hex character; advance *pp past them.
static int rdhex (char **pp) {
	char *p;
	int v, c, any;

	p = *pp;
	v = 0; any = 0;
	while (*p == ' ') ++p;
	while (1) {
		c = *p;
		if (c >= '0' && c <= '9') c = c - '0';
		else if (c >= 'a' && c <= 'f') c = c - 'a' + 10;
		else if (c >= 'A' && c <= 'F') c = c - 'A' + 10;
		else { *pp = p; return any ? v : 0; }
		v = (v << 4) | c;
		++p; any = 1;
	}
}

static void ck (int got, int want, char *op, int a, int b) {
	++total;
	// NaN sign and payload are not architecturally meaningful and this
	// library produces a canonical quiet NaN, so any NaN matches any NaN.
	if (f32_is_nan(got) && f32_is_nan(want)) return;
	if (got != want) {
		++fails;
		if (fails <= 12)
			printf("FAIL %s(0x%x, 0x%x): got 0x%x, want 0x%x\n", op, a, b, got, want);
	}
}

// Walk the generated case data and run every case.
static int run_cases (char *data) {
	char *p;
	int op, a, b, want, got;

	p = data;
	while (*p) {
		op = *p++;
		a = rdhex(&p);
		b = rdhex(&p);
		want = rdhex(&p);
		while (*p == '\n' || *p == ' ') ++p;

		got = 0;
		if      (op == 'a') got = f32_add(a, b);
		else if (op == 's') got = f32_sub(a, b);
		else if (op == 'm') got = f32_mul(a, b);
		else if (op == 'd') got = f32_div(a, b);
		else { printf("bad op '%c' in case data\n", op); return 1; }
		ck(got, want, op == 'a' ? "add" : op == 's' ? "sub" : op == 'm' ? "mul" : "div", a, b);
	}
	return 0;
}

// Conversion cases use two fields, not three.
static int run_conv (char *data) {
	char *p;
	int op, a, want, got;

	p = data;
	while (*p) {
		op = *p++;
		a = rdhex(&p);
		want = rdhex(&p);
		while (*p == '\n' || *p == ' ') ++p;

		if (op == 'i') { got = f32_from_int(a); ck(got, want, "from_int", a, 0); }
		else if (op == 't') { got = f32_to_int(a); ck(got, want, "to_int", a, 0); }
		else { printf("bad conv op '%c'\n", op); return 1; }
	}
	return 0;
}

// Specials and comparisons, checked by hand against the IEEE rules.
static void run_specials () {
	int inf, ninf, nan, zero, nzero, one, two;

	inf = F32_INF; ninf = F32_NEGINF; nan = F32_NAN;
	zero = F32_ZERO; nzero = F32_NEGZERO;
	one = f32_from_int(1); two = f32_from_int(2);

	ck(f32_add(inf, one),   inf,  "inf+1",   inf, one);
	ck(f32_add(inf, ninf),  nan,  "inf+-inf",inf, ninf);
	ck(f32_mul(inf, zero),  nan,  "inf*0",   inf, zero);
	ck(f32_div(one, zero),  inf,  "1/0",     one, zero);
	ck(f32_div(one, nzero), ninf, "1/-0",    one, nzero);
	ck(f32_div(zero, zero), nan,  "0/0",     zero, zero);
	ck(f32_add(nan, one),   nan,  "nan+1",   nan, one);
	ck(f32_add(nzero, nzero), nzero, "-0+-0", nzero, nzero);
	ck(f32_add(zero, nzero),  zero,  "0+-0",  zero, nzero);
	ck(f32_sub(one, one),     zero,  "1-1",   one, one);
	ck(f32_div(one, inf),     zero,  "1/inf", one, inf);

	// comparisons
	ck(f32_cmp(one, two), -1, "cmp 1<2", one, two);
	ck(f32_cmp(two, one),  1, "cmp 2>1", two, one);
	ck(f32_cmp(one, one),  0, "cmp 1=1", one, one);
	ck(f32_cmp(zero, nzero), 0, "cmp 0=-0", zero, nzero);
	ck(f32_cmp(f32_neg(two), f32_neg(one)), -1, "cmp -2<-1", 0, 0);
	ck(f32_cmp(nan, one), 2, "cmp nan unordered", nan, one);
	ck(f32_lt(f32_neg(one), one), 1, "lt -1<1", 0, 0);
	ck(f32_ge(two, two), 1, "ge 2>=2", 0, 0);
}

// Text conversion, checked against strings produced by C's printf("%f").
static void ckstr (char *got, char *want, char *label) {
	char *a, *b;
	++total;
	a = got; b = want;
	while (*a && (*a == *b)) { ++a; ++b; }
	if (*a != *b) {
		++fails;
		printf("FAIL %s: got '%s', want '%s'\n", label, got, want);
	}
}

static void run_text (char *buf) {
	f32_to_string(f32_from_int(0), buf, 6);          ckstr(buf, "0.000000", "str 0");
	f32_to_string(f32_from_int(1), buf, 6);          ckstr(buf, "1.000000", "str 1");
	f32_to_string(f32_from_int(-1), buf, 6);         ckstr(buf, "-1.000000", "str -1");
	f32_to_string(0x3F000000, buf, 6);               ckstr(buf, "0.500000", "str 0.5");
	f32_to_string(0x40490FDB, buf, 5);               ckstr(buf, "3.14159", "str pi");
	f32_to_string(0x3DCCCCCD, buf, 6);               ckstr(buf, "0.100000", "str 0.1");
	f32_to_string(F32_INF, buf, 6);                  ckstr(buf, "inf", "str inf");
	f32_to_string(F32_NEGINF, buf, 6);               ckstr(buf, "-inf", "str -inf");
	f32_to_string(F32_NAN, buf, 6);                  ckstr(buf, "nan", "str nan");
	f32_to_string(f32_from_int(1234), buf, 2);       ckstr(buf, "1234.00", "str 1234");

	// round trip through text
	ck(f32_from_string("1.0", 0),      f32_from_int(1),  "parse 1.0", 0, 0);
	ck(f32_from_string("-2", 0),       f32_from_int(-2), "parse -2", 0, 0);
	ck(f32_from_string("0.5", 0),      0x3F000000,       "parse 0.5", 0, 0);
	ck(f32_from_string("0.1", 0),      0x3DCCCCCD,       "parse 0.1", 0, 0);
	ck(f32_from_string("3.14159", 0),  0x40490FD0,       "parse pi", 0, 0);
	ck(f32_from_string("1e3", 0),      f32_from_int(1000), "parse 1e3", 0, 0);
	ck(f32_from_string("2.5e-1", 0),   0x3E800000,       "parse 2.5e-1", 0, 0);
}

// Show the library doing something recognisable.
static void demo (char *buf) {
	int x, y, z, i, sum;

	printf("\n-- demo --\n");

	x = f32_from_string("3.75", 0);
	y = f32_from_string("1.5", 0);
	f32_to_string(x, buf, 4); printf("x        = %s\n", buf);
	f32_to_string(y, buf, 4); printf("y        = %s\n", buf);
	f32_to_string(f32_add(x, y), buf, 4); printf("x + y    = %s\n", buf);
	f32_to_string(f32_sub(x, y), buf, 4); printf("x - y    = %s\n", buf);
	f32_to_string(f32_mul(x, y), buf, 4); printf("x * y    = %s\n", buf);
	f32_to_string(f32_div(x, y), buf, 4); printf("x / y    = %s\n", buf);

	// 1/1 + 1/2 + 1/4 + ... converges to 2
	sum = F32_ZERO;
	z = f32_from_int(1);
	i = 0;
	while (i < 20) {
		sum = f32_add(sum, z);
		z = f32_div(z, f32_from_int(2));
		++i;
	}
	f32_to_string(sum, buf, 6); printf("sum 1/2^n= %s\n", buf);

	// area of a circle, r = 2.5
	x = f32_from_string("2.5", 0);
	y = f32_from_string("3.14159265", 0);
	f32_to_string(f32_mul(y, f32_mul(x, x)), buf, 5);
	printf("pi*r^2   = %s  (r=2.5)\n", buf);
}

int main () {
	char *buf;

	total = 0; fails = 0;
	buf = malloc(64);

	if (run_cases(float_case_data())) return 1;
	if (run_conv(float_conv_data())) return 1;
	run_specials();
	run_text(buf);

	if (fails)
		printf("\n%d of %d float tests FAILED\n", fails, total);
	else
		printf("\n%d/%d float tests passed\n", total, total);

	if (!fails) demo(buf);

	free(buf);
	return fails ? 1 : 0;
}

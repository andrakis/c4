// C4 Test: the printf family built on vsnprintf.
//
// Verifies vsnprintf/snprintf/sprintf/vsprintf/vfprintf/fprintf/vprintf, and
// (when built with -DC4_PRINTF_OVERRIDE) a printf that no longer depends on
// the builtin PRTF opcode.
//
// Build and run (c4lm headers, runs under plain c4 as well as c4m):
//   gcc -E -P -Isrc/c4lm/include -DC4CC=1 -D__c4__=1 -D__c4cc__=1 src/tests/test_vprintf.c \
//     | ./c4cc -o test_vprintf.c4r -
//   ./c4m load-c4r.c -- test_vprintf.c4r
//
// With the printf override:
//   gcc -E -P -Isrc/c4lm/include -DC4CC=1 -D__c4__=1 -D__c4cc__=1 -DC4_PRINTF_OVERRIDE \
//     src/tests/test_vprintf.c | ./c4cc -o test_vprintf.c4r -

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

static char *buf;
static int fails, total;

static int streq (char *a, char *b) {
	while (*a && (*a == *b)) { ++a; ++b; }
	return *a == *b;
}

static int slen (char *s) { char *t; t = s; while (*t) ++t; return t - s; }

static void check (char *expect, int n, char *label) {
	++total;
	if (slen(expect) != n) {
		fprintf(stderr, "FAIL %s: returned %d, expected %d\n", label, n, slen(expect));
		++fails;
	} else if (!streq(expect, buf)) {
		fprintf(stderr, "FAIL %s: got '%s', expected '%s'\n", label, buf, expect);
		++fails;
	}
}

// snprintf into the shared test buffer
static int t (char *format, ...) {
	va_list args;
	int i;
	va_start(args, format);
	i = vsnprintf(buf, 256, format, args);
	va_end(args);
	return i;
}

static int tn (int size, char *format, ...) {
	va_list args;
	int i;
	va_start(args, format);
	i = vsnprintf(size ? buf : 0, size, format, args);
	va_end(args);
	return i;
}

int main () {
	int n;
	char *cap;

	buf = malloc(256);
	fails = 0; total = 0;

	// --- conversions ---
	check("hello",        t("hello"),                "plain");
	check("a%b",          t("a%%b"),                 "percent");
	check("42",           t("%d", 42),               "d");
	check("-42",          t("%d", -42),              "d-negative");
	check("0",            t("%d", 0),                "d-zero");
	check("+42",          t("%+d", 42),              "d-plus");
	check(" 42",          t("% d", 42),              "d-space");
	check("ff",           t("%x", 255),              "x");
	check("FF",           t("%X", 255),              "X");
	check("0xff",         t("%#x", 255),             "x-alt");
	check("777",          t("%o", 511),              "o");
	check("A",            t("%c", 'A'),              "c");
	check("str",          t("%s", "str"),            "s");
	check("(null)",       t("%s", 0),                "s-null");

	// --- width, precision, flags ---
	check("  42",         t("%4d", 42),              "d-width");
	check("42  ",         t("%-4d", 42),             "d-left");
	check("0042",         t("%04d", 42),             "d-zero-pad");
	check("-042",         t("%04d", -42),            "d-zero-pad-negative");
	check("00042",        t("%.5d", 42),             "d-precision");
	check("  42",         t("%*d", 4, 42),           "d-star-width");
	check("   str",       t("%6s", "str"),           "s-width");
	check("str   ",       t("%-6s", "str"),          "s-left");
	check("st",           t("%.2s", "string"),       "s-precision");

	// --- mixed and modifiers ---
	check("a1b2",         t("%s%d%s%d", "a", 1, "b", 2),  "mixed");
	check("100%",         t("%d%%", 100),            "trailing-percent");
	check("ignored 7",    t("ignored %ld", 7),       "l-modifier");

	// --- C99 return value: length that *would* have been written ---
	n = tn(4, "abcdefg");
	++total;
	if (n != 7 || !streq(buf, "abc")) {
		fprintf(stderr, "FAIL truncation: got '%s' n=%d, expected 'abc' n=7\n", buf, n);
		++fails;
	}

	// --- measuring mode: no buffer at all ---
	n = tn(0, "measure me");
	++total;
	if (n != 10) { fprintf(stderr, "FAIL measure: n=%d, expected 10\n", n); ++fails; }

	// --- the non-variadic wrappers ---
	n = snprintf(buf, 256, "%s=%d", "answer", 42);
	check("answer=42", n, "snprintf");
	n = sprintf(buf, "%s/%d", "path", 7);
	check("path/7", n, "sprintf");

	// --- redirection: the whole point of the exercise ---
	// With C4_PRINTF_OVERRIDE this captures printf() too, which is what a
	// shell needs for '>' and '|'.
	cap = malloc(256);
	__c4_stdio_capture_start(cap, 256);
	fprintf(stdout, "redirected %s #%d", "output", 1);
	fprintf(stdout, " and more");
	n = __c4_stdio_capture_stop();
	++total;
	if (!streq(cap, "redirected output #1 and more") ||
	    n != slen("redirected output #1 and more")) {
		fprintf(stderr, "FAIL capture: got '%s' (%d bytes)\n", cap, n);
		++fails;
	}

#ifdef C4_PRINTF_OVERRIDE
	// The point of the override: printf() itself becomes redirectable, which is
	// what a shell needs for '>' and '|'. Without it, printf goes straight to
	// the host via the PRTF opcode and cannot be captured.
	__c4_stdio_capture_start(cap, 256);
	printf("printf %s %d", "captured", 9);
	n = __c4_stdio_capture_stop();
	++total;
	if (!streq(cap, "printf captured 9")) {
		fprintf(stderr, "FAIL printf-capture: got '%s' (%d bytes)\n", cap, n);
		++fails;
	}
#endif

	// --- stream output; these are the ones an OS can redirect ---
	printf("printf:   plain %s and %d\n", "string", 123);
	fprintf(stdout, "fprintf:  to stdout, %d%% done\n", 50);
	fprintf(stderr, "fprintf:  to stderr\n");
	printf("captured: '%s'\n", cap);
	free(cap);

	if (fails)
		fprintf(stderr, "\n%d of %d tests FAILED\n", fails, total);
	else
		printf("\n%d/%d tests passed\n", total, total);

	free(buf);
	__c4_stdio_cleanup();
	return fails ? 1 : 0;
}

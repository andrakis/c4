//
// C4 Standard Library: stdio.h
//
// Provides the printf family built on a single formatting core, vsnprintf().
//
// Why this exists: C4 renders formatted output with the builtin PRTF opcode,
// which writes straight to the host's stdout. Nothing in the program can see
// or divert those bytes, so redirection ('>') and pipes ('|') are impossible.
// Formatting into a buffer first breaks that coupling: every function here
// ends up calling __c4_stdio_write(), which is the single place bytes leave
// the program and the one function an OS needs to replace.
//
// Availability:
//   - The formatting functions need varargs, so they require C4CC and are
//     compiled out under PURE_C4 (plain C4, used for boot.c).
//   - Only opcodes present in unmodified C4 are used, so the compiled code
//     runs under plain c4 as well as c4m.
//   - Define C4_PRINTF_OVERRIDE to additionally replace printf() itself. That
//     is opt-in because it changes the behaviour of every printf in the
//     program (see the note on the override below).
//

#ifndef __STDIO_H
#define __STDIO_H 1

#ifndef __c4cc__
#pragma GCC diagnostic ignored "-Wformat"
#pragma GCC diagnostic ignored "-Wformat-security"
#pragma GCC diagnostic ignored "-Wunused-result"
#pragma GCC diagnostic ignored "-Wbuiltin-declaration-mismatch"

int printf (const char *fmt, ...);
#endif // #ifndef __c4cc__

#ifndef PURE_C4
#include <stdarg.h>
#endif

#include <stddef.h>
#include <stdlib.h>

// Everything is an int
#define FILE int

enum { stdin, stdout, stderr };

static int OP_STDIO_FOPEN, OP_STDIO_FPUTC, OP_STDIO_FPUTS, OP_STDIO_FCLOSE;

#ifndef PURE_C4

///
/// Output layer
///

// Capture destination. While __c4_stdio_capture is non-zero, everything the
// printf family would have written is appended here instead of reaching the
// host. This is what '>' and '|' are built from: the shell points these at a
// buffer, runs the child, and then owns the bytes.
//
// Deliberately plain data rather than a callback, because calling through a
// function pointer needs the JSRI opcode, which plain C4 does not have.
static char *__c4_stdio_capture;      // destination buffer, or 0 for none
static int   __c4_stdio_capture_sz;   // capacity, including the nul
static int   __c4_stdio_capture_len;  // bytes captured so far
static int   __c4_stdio_capture_lost; // bytes dropped because the buffer filled

// Begin capturing output into 'buffer'.
static void __c4_stdio_capture_start (char *buffer, int size) {
	__c4_stdio_capture      = buffer;
	__c4_stdio_capture_sz   = size;
	__c4_stdio_capture_len  = 0;
	__c4_stdio_capture_lost = 0;
	if (buffer && size > 0) *buffer = 0;
}

// Stop capturing; returns the number of bytes captured.
static int __c4_stdio_capture_stop () {
	__c4_stdio_capture = 0;
	return __c4_stdio_capture_len;
}

// The one place formatted output actually leaves the program.
//
// This is deliberately compiled BEFORE printf() is (optionally) redefined
// below, so this call binds to C4's builtin PRTF opcode rather than recursing
// into the override.
//
// An OS with real streams should extend this to dispatch on 'stream';
// everything else in this header routes through it.
//
// @param stream  stdout, stderr, or an OS stream handle
// @param text    nul-terminated text to emit
// @param len     length of text
// @return        number of characters written
static int __c4_stdio_write (FILE *stream, char *text, int len) {
	int room, i;

	if (__c4_stdio_capture) {
		room = __c4_stdio_capture_sz - __c4_stdio_capture_len - 1;
		if (room < 0) room = 0;
		i = 0;
		while (i < len && i < room) {
			__c4_stdio_capture[__c4_stdio_capture_len + i] = text[i];
			++i;
		}
		__c4_stdio_capture_len = __c4_stdio_capture_len + i;
		__c4_stdio_capture[__c4_stdio_capture_len] = 0;
		if (i < len) __c4_stdio_capture_lost = __c4_stdio_capture_lost + (len - i);
		return i;
	}

	return printf("%s", text);
}

///
/// Formatting core
///

static int __c4_stdio_strlen (char *s) { char *t; t = s; while (*t) ++t; return t - s; }

enum { __C4_FMT_TMP_SZ = 80 };   // enough for any integer in any supported base
static char *__c4_fmt_tmp;

// Formatted output into a sized buffer.
//
// Supports: %d %i %u %x %X %o %c %s %p %%
//           flags '-' '0' '+' ' ' '#', a field width, a '.precision', and '*'
//           for either. Length modifiers (l, ll, h, hh, z, t, j) are accepted
//           and ignored, because every C4 value is exactly one word.
//           Floating point is not supported; C4 has no float type.
//
// Follows C99: writes at most size-1 characters plus a nul terminator, and
// returns the length the result *would* have had. Passing str = 0, size = 0
// therefore measures a format without writing anything, which is how the
// functions below size their buffers.
//
// Not reentrant: a small scratch buffer is shared between calls, so do not
// call this from a signal handler that could interrupt another call.
static int vsnprintf (char *str, size_t size, char *format, va_list args) {
	char *f, *s, *dig, *out, *tmp, *prefix;
	int count, room, run;
	int c, v, base, uc, neg;
	int left, zero, plus, space, alt;
	int width, prec, haveprec;
	int i, j, len, pad, plen;
	int bits, smask, d, q, r;

	if (!__c4_fmt_tmp) {
		if (!(__c4_fmt_tmp = malloc(__C4_FMT_TMP_SZ)))
			return 0;
	}

	f     = format;
	out   = str;
	tmp   = __c4_fmt_tmp;
	count = 0;
	// Characters we may still store, not counting the terminating nul.
	room  = (str != 0 && size > 0) ? size - 1 : 0;
	bits  = sizeof(int) * 8;
	// 0x7fff...f -- ANDing after a right shift clears the sign extension,
	// which is how unsigned conversions are done without an unsigned type.
	smask = (1 << (bits - 1)) - 1;

	while (*f) {
		if (*f != '%') {
			if (room) { *out++ = *f; --room; }
			++count;
			++f;
		} else {
			++f;

			// ---- flags ----
			left = 0; zero = 0; plus = 0; space = 0; alt = 0;
			run = 1;
			while (run) {
				if      (*f == '-') { left  = 1; ++f; }
				else if (*f == '0') { zero  = 1; ++f; }
				else if (*f == '+') { plus  = 1; ++f; }
				else if (*f == ' ') { space = 1; ++f; }
				else if (*f == '#') { alt   = 1; ++f; }
				else run = 0;
			}

			// ---- field width ----
			width = 0;
			if (*f == '*') {
				width = va_arg(args, int); ++f;
				if (width < 0) { left = 1; width = 0 - width; }
			} else
				while (*f >= '0' && *f <= '9') { width = width * 10 + (*f - '0'); ++f; }

			// ---- precision ----
			haveprec = 0; prec = 0;
			if (*f == '.') {
				++f; haveprec = 1;
				if (*f == '*') {
					prec = va_arg(args, int); ++f;
					if (prec < 0) haveprec = 0;
				} else
					while (*f >= '0' && *f <= '9') { prec = prec * 10 + (*f - '0'); ++f; }
			}

			// ---- length modifiers: every value is one word, so ignore ----
			while (*f == 'l' || *f == 'h' || *f == 'z' || *f == 't' || *f == 'j') ++f;

			c = *f;
			if (c) ++f;

			// ---- build the conversion body in s/len, plus prefix/plen ----
			s = 0; len = 0; prefix = ""; plen = 0; base = 0;

			if (c == '%') {
				*tmp = '%'; s = tmp; len = 1; zero = 0;
			} else if (c == 'c') {
				*tmp = (char)va_arg(args, int); s = tmp; len = 1; zero = 0;
			} else if (c == 's') {
				s = (char *)va_arg(args, int);
				if (!s) s = "(null)";
				len = __c4_stdio_strlen(s);
				if (haveprec && prec < len) len = prec;
				zero = 0;
			} else if (c == 'd' || c == 'i' || c == 'u' ||
			           c == 'x' || c == 'X' || c == 'o' || c == 'p') {
				v   = va_arg(args, int);
				uc  = (c == 'X');
				neg = 0;
				if (c == 'd' || c == 'i') { base = 10; if (v < 0) neg = 1; }
				else if (c == 'u') base = 10;
				else if (c == 'o') base = 8;
				else base = 16;

				if (c == 'p') { prefix = "0x"; plen = 2; }
				else if (alt && base == 16 && v != 0) { prefix = uc ? "0X" : "0x"; plen = 2; }
				else if (neg)   { prefix = "-"; plen = 1; }
				else if (plus)  { prefix = "+"; plen = 1; }
				else if (space) { prefix = " "; plen = 1; }

				dig = uc ? "0123456789ABCDEF" : "0123456789abcdef";

				// Digits are produced least-significant first, then reversed.
				i = 0;
				if (v == 0) {
					if (!(haveprec && prec == 0)) { tmp[i] = '0'; ++i; }
				} else if (base == 10 && c == 'u') {
					// Unsigned decimal with no unsigned type: divide by 10 via a
					// logical shift, then correct the remainder.
					while (v != 0) {
						q = ((v >> 1) & smask) / 5;
						r = v - q * 10;
						if (r > 9) { q = q + 1; r = r - 10; }
						tmp[i] = dig[r]; ++i;
						v = q;
					}
				} else if (base == 10) {
					// Signed: never negate, so the most negative value converts
					// correctly instead of overflowing.
					while (v != 0) {
						d = v % 10; if (d < 0) d = 0 - d;
						tmp[i] = dig[d]; ++i;
						v = v / 10;
					}
				} else if (base == 16) {
					while (v != 0) { tmp[i] = dig[v & 15]; ++i; v = (v >> 4) & (smask >> 3); }
				} else {
					while (v != 0) { tmp[i] = dig[v & 7]; ++i; v = (v >> 3) & (smask >> 2); }
				}
				// Precision on an integer means "at least this many digits".
				while (haveprec && i < prec) { tmp[i] = '0'; ++i; }
				if (haveprec) zero = 0;

				len = i;
				i = 0; j = len - 1;
				while (i < j) {
					d = tmp[i]; tmp[i] = tmp[j]; tmp[j] = (char)d;
					++i; --j;
				}
				s = tmp;
			}

			// ---- emit, applying width and padding ----
			if (c) {
				pad = width - len - plen;
				if (pad < 0) pad = 0;

				if (!left && !zero)
					while (pad > 0) { if (room) { *out++ = ' '; --room; } ++count; --pad; }

				i = 0;
				while (i < plen) { if (room) { *out++ = prefix[i]; --room; } ++count; ++i; }

				if (!left && zero)
					while (pad > 0) { if (room) { *out++ = '0'; --room; } ++count; --pad; }

				i = 0;
				while (i < len) { if (room) { *out++ = s[i]; --room; } ++count; ++i; }

				if (left)
					while (pad > 0) { if (room) { *out++ = ' '; --room; } ++count; --pad; }
			}
		}
	}

	if (str != 0 && size > 0) *out = 0;
	return count;
}

///
/// Buffer-producing wrappers
///

static int vsprintf (char *str, char *format, va_list args) {
	// No bound. Prefer vsnprintf().
	return vsnprintf(str, 0x7FFFFFFF, format, args);
}

static int snprintf (char *str, size_t size, char *format, ...) {
	va_list args;
	int i;

	va_start(args, format);
	i = vsnprintf(str, size, format, args);
	va_end(args);

	return i;
}

static int sprintf (char *str, char *format, ...) {
	va_list args;
	int i;

	va_start(args, format);
	i = vsnprintf(str, 0x7FFFFFFF, format, args);
	va_end(args);

	return i;
}

///
/// Stream output
///

enum { __C4_STDIO_BUFSZ = 256 };
static char *__c4_stdio_buf;
static int   __c4_stdio_bufsz;

// Render into the shared output buffer, growing it if the result does not fit,
// then hand the bytes to __c4_stdio_write().
static int vfprintf (FILE *stream, char *format, va_list args) {
	va_list measure;
	int n;

	// Measure first so the buffer can be sized exactly. va_copy is needed
	// because the measuring pass consumes the argument list.
	va_copy(measure, args);
	n = vsnprintf(0, 0, format, measure);

	if (n + 1 > __c4_stdio_bufsz) {
		if (__c4_stdio_buf) free(__c4_stdio_buf);
		__c4_stdio_bufsz = n + 1;
		if (__c4_stdio_bufsz < __C4_STDIO_BUFSZ) __c4_stdio_bufsz = __C4_STDIO_BUFSZ;
		if (!(__c4_stdio_buf = malloc(__c4_stdio_bufsz))) {
			__c4_stdio_bufsz = 0;
			return 0;
		}
	}

	vsnprintf(__c4_stdio_buf, __c4_stdio_bufsz, format, args);
	__c4_stdio_write(stream, __c4_stdio_buf, n);

	return n;
}

static int fprintf (FILE *stream, char *format, ...) {
	va_list args;
	int i;

	va_start(args, format);
	i = vfprintf(stream, format, args);
	va_end(args);

	return i;
}

static int vprintf (char *format, va_list args) {
	return vfprintf(stdout, format, args);
}

// Replacing printf() itself is opt-in. It routes every printf in the program
// through the code above, which is what makes output redirectable -- but it
// also means printf gains this header's limits (no floating point, and the
// argument count is no longer capped at PRTF's 7).
#ifdef C4_PRINTF_OVERRIDE
static int printf (char *format, ...) {
	va_list args;
	int i;

	va_start(args, format);
	i = vfprintf(stdout, format, args);
	va_end(args);

	return i;
}
#endif /* ifdef C4_PRINTF_OVERRIDE */

// Release the buffers this header allocates lazily. Optional; the OS reclaims
// the memory on exit.
static void __c4_stdio_cleanup () {
	if (__c4_fmt_tmp)   { free(__c4_fmt_tmp);   __c4_fmt_tmp = 0; }
	if (__c4_stdio_buf) { free(__c4_stdio_buf); __c4_stdio_buf = 0; __c4_stdio_bufsz = 0; }
}

#endif /* ifndef PURE_C4 */

#endif /* ifndef __STDIO_H */

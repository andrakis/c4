# 0 "src/tests/test_basic.c"
# 0 "<built-in>"
# 0 "<command-line>"
# 1 "/usr/include/stdc-predef.h" 1 3 4

# 1 "/usr/include/stdc-predef.h" 3 4
/* Copyright (C) 1991-2026 Free Software Foundation, Inc.
   This file is part of the GNU C Library.

   The GNU C Library is free software; you can redistribute it and/or
   modify it under the terms of the GNU Lesser General Public
   License as published by the Free Software Foundation; either
   version 2.1 of the License, or (at your option) any later version.

   The GNU C Library is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Lesser General Public License for more details.

   You should have received a copy of the GNU Lesser General Public
   License along with the GNU C Library; if not, see
   <https://www.gnu.org/licenses/>.  */




/* This header is separate from features.h so that the compiler can
   include it implicitly at the start of every compilation.  It must
   not itself include <features.h> or any other header that includes
   <features.h> because the implicit include comes before any feature
   test macros that may be defined in a source file before it first
   explicitly includes a system header.  GCC knows the name of this
   header in order to preinclude it.  */

/* glibc's intent is to support the IEC 559 math functionality, real
   and complex.  If the GCC (4.9 and later) predefined macros
   specifying compiler intent are available, use them to determine
   whether the overall intent is to support these features; otherwise,
   presume an older compiler has intent to support these features and
   define these macros by default.  */
# 56 "/usr/include/stdc-predef.h" 3 4
/* wchar_t uses Unicode 10.0.0.  Version 10.0 of the Unicode Standard is
   synchronized with ISO/IEC 10646:2017, fifth edition, plus
   the following additions from Amendment 1 to the fifth edition:
   - 56 emoji characters
   - 285 hentaigana
   - 3 additional Zanabazar Square characters */
# 0 "<command-line>" 2
# 1 "src/tests/test_basic.c"

# 1 "src/tests/test_basic.c"
// A basic test
# 1 "include/stdio.h" 1
//
// C4KE Standard Library: stdio.h
//
// Provides the printf family built on a single formatting core, vsnprintf().
//
// Why this exists: C4 renders formatted output with the builtin PRTF opcode,
// which writes straight to the host's stdout. Nothing in the program can see
// or divert those bytes, so redirection ('>') and pipes ('|') are impossible.
// Formatting into a buffer first breaks that coupling: every function here
// ends up calling __c4_stdio_write(), which is the single place bytes leave
// the program and the one function C4KE needs to point at a stream.
//
// Only opcodes present in unmodified C4 are emitted, so code built against
// this header runs under plain c4 as well as c4m.
//
// Define C4_PRINTF_OVERRIDE to additionally replace printf() itself. That is
// opt-in because it changes the behaviour of every printf in the program (see
// the note on the override below).
//
// NOTE: src/c4lm/include/stdio.h carries an equivalent implementation for
//       c4lm, which has its own isolated include tree. The formatting core is
//       identical; only the output layer differs. Worth consolidating if the
//       two trees ever merge.
//
# 34 "include/stdio.h"
# 1 "include/c4ke/common.h" 1
//
// C4KE Standard Library: c4ke/common.h
// Common definitions used by C4KE user-mode programs.
// Includes stream control (read-only for now)
# 20 "include/c4ke/common.h"
# 1 "include/c4ke/opcodes.h" 1
//
// C4KE Standard Library: c4ke/opcodes.h
// Provides the interface to C4KE opcodes.
# 12 "include/c4ke/opcodes.h"
// User-mode specific version of opcodes interface

// C4KE opcode: int request_opcode(char *name)
enum { OP_REQUEST_SYMBOL = 128 };

static int c4ke_opcode(char *opcode) {
 return __c4_opcode(opcode, OP_REQUEST_SYMBOL);
}
# 21 "include/c4ke/common.h" 2

static int OP_STREAM_DEST, OP_STREAM_UPDATE;
# 35 "include/stdio.h" 2
# 1 "include/stdarg.h" 1
//
// C4 Standard Library: stdarg.h
//
// Adds the usual va_list type, va_arg, va_start, va_end, and va_copy.
//




// Don't read this as gcc




# 1 "include/stddef.h" 1
//
// C4KE Standard Library: stddef.h
// Provides standard definitions.
# 12 "include/stddef.h"
// Everything is an int







// unsigned is ignored under C4CC.
// Always use the full type name, ie `unsigned int` instead of just `unsigned`.
# 16 "include/stdarg.h" 2
# 1 "include/stdlib.h" 1
//
// C4KE Standard Library: stdlib.h
//
# 14 "include/stdlib.h"
# 1 "include/c4ke/common.h" 1
//
// C4KE Standard Library: c4ke/common.h
// Common definitions used by C4KE user-mode programs.
// Includes stream control (read-only for now)
# 15 "include/stdlib.h" 2
# 1 "include/stddef.h" 1
//
// C4KE Standard Library: stddef.h
// Provides standard definitions.
# 16 "include/stdlib.h" 2

static int OP_STDLIB_MALLOC, OP_STDLIB_FREE;
# 29 "include/stdlib.h"
void *calloc (int nmemb, int size) {
 int total_sz, new_sz;
 void *ptr;

 // Make sure we don't overflow
 total_sz = 0;
 while (nmemb) {
  if ((new_sz = total_sz + size) < 0 || new_sz < total_sz) {
   // Overflow;
   return 0;
  }
  total_sz = new_sz;
  --nmemb;
 }

 if ((ptr = malloc(total_sz))) {
  memset(ptr, total_sz, 0);
 }

 return ptr;
}
# 17 "include/stdarg.h" 2
# 1 "include/string.h" 1
//
// C4 Standard Library: string.h
//
# 19 "include/string.h"
static int strlen (char *s) { int i; i = 0; while (*s++) ++i; return i; }
// static void *memcpy (void *source, void *dest, int length) {
// 	int   i;
// 	int  *is, *id;
// 	char *cs, *cd;
// 
// 	i = 0;
// 	if((int)dest   % sizeof(int) == 0 &&
// 	   (int)source % sizeof(int) == 0 &&
// 	   length % sizeof(int) == 0) {
// 		is = source; id = dest;
// 		length = length / sizeof(int);
// 		while (i < length) { id[i] = is[i]; ++i; }
// 	} else {
// 		cs = source; cd = dest;
// 		while (i < length) { cd[i] = cs[i]; ++i; }
// 	}
// 
// 	return dest;
// }
static void *memmove (void *source, void *dest, int length) {
 int i;
 int *is, *id;
 char *cs, *cd;

 if ((int)dest < (int)source)
  return memcpy(dest, source, length);

 i = length;
 if((int)dest % sizeof(int) == 0 &&
    (int)source % sizeof(int) == 0 &&
    length % sizeof(int) == 0) {
  is = source; id = dest;
  length = length / sizeof(int);
  while (i > 0) { id[i - 1] = is[i - 1]; --i; }
 } else {
  cs = source; cd = dest;
  while (i > 0) { cd[i - 1] = cs[i - 1]; --i; }
 }

 return dest;
}
# 18 "include/stdarg.h" 2


static int *__c4cc_va_stack;
static int __c4cc_va_vptr;

// Adjustment of the __c4cc_va_stack and __c4cc_va_vptr variables.
// Note: the first macro is a multi-statement macro, but not enclosed in a
// do/while(0) loop, as C4CC doesn't understand this construct.




// The traditional va_arg and related macros. These use the above adjustment macros.


// va_start does an implicit va_arg to skip the count.
// va_end undoes this to get back to the count.




// These variables are used by __c4cc_make_va, as local variables cannot be used.
static int *__c4cc_va_m_ptr, *__c4cc_va_m_arg, __c4cc_va_m_n, __c4cc_va_m_v;

// A call to this function is inserted by C4CC into code that calls variadic functions,
// along with the parameter count.
// The above globals are required as our bp is at an unknown offset, depending
// on number of arguments pushed. Using local variables would overwrite values
// on the stack.
// Globals on the other hand, are hard-coded addresses instead of references to bp.
// The count parameter still works, as it references the last item on the stack.
// Similarly, variadic functions can still use their arguments as normal.
static int *__c4cc_make_va (int count) {
 __c4cc_va_m_n = count;
 __c4cc_va_m_arg = &count + count;
 __c4cc_va_m_ptr = &__c4cc_va_stack[__c4cc_va_vptr];

 // Push the count as the last argument. This is used by va_end.
 __c4cc_va_stack[__c4cc_va_vptr] = (count); ++__c4cc_va_vptr;

 // Push all arguments in reverse order
 while (__c4cc_va_m_n--) {
  __c4cc_va_stack[__c4cc_va_vptr] = (*__c4cc_va_m_arg); ++__c4cc_va_vptr;
  --__c4cc_va_m_arg;
 }

 // Return the start of the list
 return __c4cc_va_m_ptr;
}

// Constructor added to allocate the var args stack.
static void __attribute__((constructor)) __c4cc_va_constructor () {
 int i;
 if (!(__c4cc_va_stack = malloc(i = sizeof(int) * 64))) {
  printf("stdarg.h: out of memory attempting to allocate %d bytes\n", i);
  exit(-100);
 }
 memset(__c4cc_va_stack, 0, i);
 __c4cc_va_vptr = 0;
}

// Destructor to free the var args stack.
static void __attribute__((destructor)) __c4cc_va_destructor () {
 if (__c4cc_va_stack) free(__c4cc_va_stack);
}
# 36 "include/stdio.h" 2
# 1 "include/stddef.h" 1
//
// C4KE Standard Library: stddef.h
// Provides standard definitions.
# 37 "include/stdio.h" 2
# 1 "include/stdlib.h" 1
//
// C4KE Standard Library: stdlib.h
//
# 38 "include/stdio.h" 2

// Everything is an int


enum { stdin, stdout, stderr };

static int OP_STDIO_FOPEN, OP_STDIO_FPUTC, OP_STDIO_FPUTS, OP_STDIO_FCLOSE;
# 78 "include/stdio.h"
///
/// Output layer
///

// Capture destination. While __c4_stdio_capture is non-zero, everything the
// printf family would have written is appended here instead of being emitted.
// This is what '>' and '|' are built from: the shell points these at a buffer,
// runs the child, and then owns the bytes.
//
// Deliberately plain data rather than a callback, because calling through a
// function pointer needs the JSRI opcode, which plain C4 does not have.
static char *__c4_stdio_capture; // destination buffer, or 0 for none
static int __c4_stdio_capture_sz; // capacity, including the nul
static int __c4_stdio_capture_len; // bytes captured so far
static int __c4_stdio_capture_lost; // bytes dropped because the buffer filled

// Begin capturing output into 'buffer'.
static void __c4_stdio_capture_start (char *buffer, int size) {
 __c4_stdio_capture = buffer;
 __c4_stdio_capture_sz = size;
 __c4_stdio_capture_len = 0;
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
// below, so its own printf call binds to C4's builtin PRTF opcode rather than
// recursing into the override.
//
// C4KE's stream opcodes (OP_STREAM_DEST) are not implemented in the kernel
// yet; when they are, dispatch on 'stream' here and everything above follows.
//
// @param stream  stdout, stderr, or a C4KE stream handle
// @param text    nul-terminated text to emit
// @param len     length of text
// @return        number of characters written
static int __c4_stdio_write (int *stream, char *text, int len) {
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

enum { __C4_FMT_TMP_SZ = 80 }; // enough for any integer in any supported base
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
static int vsnprintf (char *str, int size, char *format, int * args) {
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

 f = format;
 out = str;
 tmp = __c4_fmt_tmp;
 count = 0;
 // Characters we may still store, not counting the terminating nul.
 room = (str != 0 && size > 0) ? size - 1 : 0;
 bits = sizeof(int) * 8;
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
    if (*f == '-') { left = 1; ++f; }
    else if (*f == '0') { zero = 1; ++f; }
    else if (*f == '+') { plus = 1; ++f; }
    else if (*f == ' ') { space = 1; ++f; }
    else if (*f == '#') { alt = 1; ++f; }
    else run = 0;
   }

   // ---- field width ----
   width = 0;
   if (*f == '*') {
    width = (args = args + 1, *((int *) (args - 1))); ++f;
    if (width < 0) { left = 1; width = 0 - width; }
   } else
    while (*f >= '0' && *f <= '9') { width = width * 10 + (*f - '0'); ++f; }

   // ---- precision ----
   haveprec = 0; prec = 0;
   if (*f == '.') {
    ++f; haveprec = 1;
    if (*f == '*') {
     prec = (args = args + 1, *((int *) (args - 1))); ++f;
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
    *tmp = (char)(args = args + 1, *((int *) (args - 1))); s = tmp; len = 1; zero = 0;
   } else if (c == 's') {
    s = (char *)(args = args + 1, *((int *) (args - 1)));
    if (!s) s = "(null)";
    len = __c4_stdio_strlen(s);
    if (haveprec && prec < len) len = prec;
    zero = 0;
   } else if (c == 'd' || c == 'i' || c == 'u' ||
              c == 'x' || c == 'X' || c == 'o' || c == 'p') {
    v = (args = args + 1, *((int *) (args - 1)));
    uc = (c == 'X');
    neg = 0;
    if (c == 'd' || c == 'i') { base = 10; if (v < 0) neg = 1; }
    else if (c == 'u') base = 10;
    else if (c == 'o') base = 8;
    else base = 16;

    if (c == 'p') { prefix = "0x"; plen = 2; }
    else if (alt && base == 16 && v != 0) { prefix = uc ? "0X" : "0x"; plen = 2; }
    else if (neg) { prefix = "-"; plen = 1; }
    else if (plus) { prefix = "+"; plen = 1; }
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

static int vsprintf (char *str, char *format, int * args) {
 // No bound. Prefer vsnprintf().
 return vsnprintf(str, 0x7FFFFFFF, format, args);
}

static int snprintf (char *str, int size, char *format, ...) {
 int * args;
 int i;

 (args = *(&format - 1), (args = args + 1, *((int *) (args - 1))));
 i = vsnprintf(str, size, format, args);
 (args = args - 1, __c4cc_va_vptr = __c4cc_va_vptr - (1 + (args = args + 1, *((int *) (args - 1)))));

 return i;
}

static int sprintf (char *str, char *format, ...) {
 int * args;
 int i;

 (args = *(&format - 1), (args = args + 1, *((int *) (args - 1))));
 i = vsnprintf(str, 0x7FFFFFFF, format, args);
 (args = args - 1, __c4cc_va_vptr = __c4cc_va_vptr - (1 + (args = args + 1, *((int *) (args - 1)))));

 return i;
}

///
/// Stream output
///

enum { __C4_STDIO_BUFSZ = 256 };
static char *__c4_stdio_buf;
static int __c4_stdio_bufsz;

// Render into the shared output buffer, growing it if the result does not fit,
// then hand the bytes to __c4_stdio_write().
static int vfprintf (int *stream, char *format, int * args) {
 int * measure;
 int n;

 // Measure first so the buffer can be sized exactly. va_copy is needed
 // because the measuring pass consumes the argument list.
 (measure = (args));
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

static int fprintf (int *stream, char *format, ...) {
 int * args;
 int i;

 (args = *(&format - 1), (args = args + 1, *((int *) (args - 1))));
 i = vfprintf(stream, format, args);
 (args = args - 1, __c4cc_va_vptr = __c4cc_va_vptr - (1 + (args = args + 1, *((int *) (args - 1)))));

 return i;
}

static int vprintf (char *format, int * args) {
 return vfprintf(stdout, format, args);
}

// Replacing printf() itself is opt-in. It routes every printf in the program
// through the code above, which is what makes output redirectable -- but it
// also means printf gains this header's limits (no floating point) and loses
// PRTF's speed and its 7-argument ceiling.
# 432 "include/stdio.h"
// Release the buffers this header allocates lazily. Optional; C4KE reclaims
// the memory when the task exits.
static void __c4_stdio_cleanup () {
 if (__c4_fmt_tmp) { free(__c4_fmt_tmp); __c4_fmt_tmp = 0; }
 if (__c4_stdio_buf) { free(__c4_stdio_buf); __c4_stdio_buf = 0; __c4_stdio_bufsz = 0; }
}



#pragma GCC diagnostic ignored "-Wformat"
#pragma GCC diagnostic ignored "-Wformat-security"
#pragma GCC diagnostic ignored "-Wunused-result"
# 3 "src/tests/test_basic.c" 2

int add (int a, int b) { return a + b; }

int main (int argc, char **argv) {
  int a, b, c;
  a = 2;
  b = 3;
  printf("A basic test of %d + %d:\n", a, b);
  c = add(a, b);
  printf("  %d\n", c);
  return 0;
}

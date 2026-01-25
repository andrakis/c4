//
// C4KE Standard Library: stdio.h
//

#ifndef __STDIO_H
#define __STDIO_H 1

#ifndef __c4cc__
#include_next </usr/include/stdio.h>
#else /* ifndef __c4cc__ */
#ifndef C4KE

#include <c4ke/common.h>
#include <stdarg.h>

// Everything is an int
#define FILE int

enum { stdin, stdout, stderr };

static int OP_STDIO_FOPEN, OP_STDIO_FPUTC, OP_STDIO_FPUTS, OP_STDIO_FCLOSE;

#if STDLIB_EXPERIMENTAL
#if NO_INLINE
static int *fopen (char *p, char *m) { return __c4_opcode(m, p, OP_STDIO_FOPEN); }
static int fputc (int c, FILE *str)  { return __c4_opcode(str, c, OP_STDIO_FPUTC); }
static int fputs (char *s, FILE *str){ return __c4_opcode(str, s, OP_STDIO_FPUTS); }
static int fclose (FILE *f)          { return __c4_opcode(f, OP_STDIO_FCLOSE);
static int putchar (int c)           { return fputc(c, stdout); }
#else /* if NO_INLINE */
#define fopen(p,m)                    __c4_opcode(m, p, OP_STDIO_FOPEN)
#define fputc(c,str)                  __c4_opcode(str, c, OP_STDIO_FPUTC)
#define fputs(s,str)                  __c4_opcode(str, s, OP_STDIO_FPUTS)
#define putchar(c)                    fputc(c, stdout)
#endif /* if NO_INLINE */
// Can't be a macro
static int puts  (char *s)           { return fputs(s, stdout); putchar('\n'); } // required by standard

int OP_STDIO_FOPEN, OP_STDIO_FPUTC, OP_STDIO_FPUTS, OP_STDIO_FCLOSE;
static int __attribute__((constructor)) __stdio_init (int *c4r) {
	OP_STDIO_FOPEN = c4ke_opcode("OP_STDIO_FOPEN");
	OP_STDIO_FPUTC = c4ke_opcode("OP_STDIO_FPUTC");
	OP_STDIO_FPUTS = c4ke_opcode("OP_STDIO_FPUTS");
	OP_STDIO_FCLOSE = c4ke_opcode("OP_STDIO_FCLOSE");
}
#endif /* if STDLIB_EXPERIMENTAL */

// All v(s)(n)(f)printf functions use vsnprintf internally.
static int vsnprintf (char *str, size_t size, char *format, va_list args) {
	printf("TODO: vsnprintf not implemented yet\n");
	return 0;
}

static int snprintf (char *str, size_t size, char *format, ...) {
	va_list args;
	int i;

	va_start(args, format);
	i = vsnprintf(str, size, format, args);
	va_end(args);

	return i;
}

static int sprintf (char *str, size_t size, char *format, ...) {
	va_list args;
	int i;

	va_start(args, format);
	i = vsnprintf(str, -1, format, args);
	va_end(args);

	return i;
}

static int vfprintf (FILE *stream, char *format, va_list args) {
	char *dest;
	int   stream_avail;
	int   i;

	dest = __c4ke_stream_dest(stream, &stream_avail);
	i = vsnprintf(dest, stream_avail, format, args);
	__c4ke_stream_update(stream);

	return i;
}

static int fprintf (FILE *stream, char *format, ...) {
	va_list args;
	int i;

	va_start(args, format);
	i = vfprintf(stream, format, args);
	va_end(args);

	return i;
}

#if NO_INLINE
static int vprintf (char *format, va_list args) {
	return vfprintf(stdout, format, ap);
}
#else
#define vprintf(f,ap) vfprintf(stdout, format, ap)
#endif

#if STDLIB_EXPERIMENTAL
static int printf (char *format, ...) {
	va_list args;
	int i;

	va_start(args, format);
	i = vprintf(format, args);
	va_end(args);

	return i;
}
#endif /* if STDLIB_EXPERIMENTAL */

#endif /* ifndef C4KE */
#endif /* ifndef/else __c4cc__ */
#pragma GCC diagnostic ignored "-Wformat"
#pragma GCC diagnostic ignored "-Wformat-security"
#pragma GCC diagnostic ignored "-Wunused-result"

#endif /* ifndef __STDLIB_H */

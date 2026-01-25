//
// C4 Standard Library: stdio.h
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

// Everything is an int
#define FILE int

enum { stdin, stdout, stderr };

static int OP_STDIO_FOPEN, OP_STDIO_FPUTC, OP_STDIO_FPUTS, OP_STDIO_FCLOSE;

// Can't be a macro
//static int puts  (char *s)           { return fputs(s, stdout); putchar('\n'); } // required by standard

//#ifndef PURE_C4
#ifdef EXPERIMENTAL_STDIO
// All v(s)(n)(f)printf functions use vsnprintf internally.
static int vsnprintf (char *str, size_t size, char *format, va_list args) {
	printf("TODO: vsnprintf not implemented yet\n");
	return 0;
}

static int snprintf (char *str, size_t size, char *format, ...) {
	va_list args;
	int i;

	va_start(args, format);
	// TODO: c4cc thinks this call is not the correct number of args
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

	//TODO
	//dest = __c4ke_stream_dest(stream, &stream_avail);
	i = vsnprintf(dest, stream_avail, format, args);
	//__c4ke_stream_update(stream);

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

#endif // #ifndef PURE_C4

#endif /* ifndef __STDLIB_H */

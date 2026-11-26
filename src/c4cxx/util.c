// util.c -- memory, messages, files and buffers
#include "c4cxx.h"

void *xalloc (size_t n)
{
	void *p = calloc (1, n ? n : 1);
	if (!p)
		fatal ("out of memory (%lu bytes)", (unsigned long)n);
	return p;
}

char *xstrndup (const char *s, int n)
{
	char *p = xalloc (n + 1);
	memcpy (p, s, n);
	p[n] = 0;
	return p;
}

char *xformat (const char *fmt, ...)
{
	Buf b = {0};
	va_list ap;
	char tmp[1024];
	int n;
	va_start (ap, fmt);
	n = vsnprintf (tmp, sizeof tmp, fmt, ap);
	va_end (ap);
	if (n < (int)sizeof tmp) {
		buf_add (&b, tmp, n);
		return b.p;
	}
	b.p = xalloc (n + 1);
	va_start (ap, fmt);
	vsnprintf (b.p, n + 1, fmt, ap);
	va_end (ap);
	return b.p;
}

void fatal (const char *fmt, ...)
{
	va_list ap;
	fprintf (stderr, "c4cxx: ");
	va_start (ap, fmt);
	vfprintf (stderr, fmt, ap);
	va_end (ap);
	fprintf (stderr, "\n");
	exit (1);
}

static void message (Token *t, const char *what, const char *fmt, va_list ap)
{
	if (t && t->file)
		fprintf (stderr, "%s:%d: %s: ", t->file->name, t->line, what);
	else
		fprintf (stderr, "c4cxx: %s: ", what);
	vfprintf (stderr, fmt, ap);
	if (t && t->kind != TK_EOF && t->len > 0 && t->len < 60)
		fprintf (stderr, " (at '%.*s')", t->len, t->loc);
	fprintf (stderr, "\n");
}

void error_at (Token *t, const char *fmt, ...)
{
	va_list ap;
	va_start (ap, fmt);
	message (t, "error", fmt, ap);
	va_end (ap);
	exit (1);
}

void warn_at (Token *t, const char *fmt, ...)
{
	va_list ap;
	va_start (ap, fmt);
	message (t, "warning", fmt, ap);
	va_end (ap);
}

char *read_file (const char *path)
{
	FILE *f = fopen (path, "rb");
	Buf b = {0};
	char tmp[65536];
	size_t n;
	if (!f)
		return 0;
	while ((n = fread (tmp, 1, sizeof tmp, f)) > 0)
		buf_add (&b, tmp, (int)n);
	fclose (f);
	buf_add (&b, "", 0);
	return b.p;
}

void buf_add (Buf *b, const char *s, int n)
{
	if (b->n + n + 1 > b->cap) {
		int cap = b->cap ? b->cap * 2 : 256;
		char *p;
		while (cap < b->n + n + 1)
			cap *= 2;
		p = xalloc (cap);
		if (b->p)
			memcpy (p, b->p, b->n);
		free (b->p);
		b->p = p;
		b->cap = cap;
	}
	memcpy (b->p + b->n, s, n);
	b->n += n;
	b->p[b->n] = 0;
}

void buf_str (Buf *b, const char *s)
{
	buf_add (b, s, (int)strlen (s));
}

void buf_char (Buf *b, int c)
{
	char ch = (char)c;
	buf_add (b, &ch, 1);
}

void buf_fmt (Buf *b, const char *fmt, ...)
{
	char tmp[2048];
	va_list ap;
	int n;
	va_start (ap, fmt);
	n = vsnprintf (tmp, sizeof tmp, fmt, ap);
	va_end (ap);
	if (n >= (int)sizeof tmp)
		n = sizeof tmp - 1;
	buf_add (b, tmp, n);
}

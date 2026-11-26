// lex.c -- source text to preprocessing tokens
#include "c4cxx.h"

static int file_count;

int is_ident_start (int c)
{
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '$';
}

int is_ident_char (int c)
{
	return is_ident_start (c) || (c >= '0' && c <= '9');
}

// CR LF and lone CR become LF; a backslash before a line end (LF or
// CR LF, as MSVC-era sources have it) joins the lines, and the joined
// line ends are put back after the logical line so that line numbers stay
// those of the file.
static char *canonical (char *s)
{
	Buf b = {0};
	int pending = 0, i = 0;
	while (s[i]) {
		if (s[i] == '\\' && (s[i + 1] == '\n' || (s[i + 1] == '\r' && s[i + 2] == '\n') || s[i + 1] == '\r')) {
			i += (s[i + 1] == '\r' && s[i + 2] == '\n') ? 3 : 2;
			pending++;
			continue;
		}
		if (s[i] == '\r') {
			buf_char (&b, '\n');
			i += s[i + 1] == '\n' ? 2 : 1;
		} else
			buf_char (&b, s[i++]);
		if (b.p[b.n - 1] == '\n') {
			for (; pending; pending--)
				buf_char (&b, '\n');
		}
	}
	if (b.n == 0 || b.p[b.n - 1] != '\n')
		buf_char (&b, '\n');
	for (; pending; pending--)
		buf_char (&b, '\n');
	return b.p;
}

File *new_file (const char *name, char *text)
{
	File *f = xalloc (sizeof *f);
	const char *slash = strrchr (name, '/');
	f->name = xstrndup (name, (int)strlen (name));
	f->dir = slash ? xstrndup (name, (int)(slash - name) + 1) : xstrndup ("", 0);
	f->text = text ? canonical (text) : 0;
	f->index = ++file_count;
	return f;
}

Token *copy_token (Token *t)
{
	Token *n = xalloc (sizeof *n);
	*n = *t;
	n->next = 0;
	return n;
}

int equal (Token *t, const char *s)
{
	int n = (int)strlen (s);
	return t->len == n && !memcmp (t->loc, s, n) && t->kind != TK_STR && t->kind != TK_CHAR;
}

static const char *puncts[] = {
	"<<=", ">>=", "...", "->*",
	"==", "!=", "<=", ">=", "->", "++", "--", "<<", ">>", "&&", "||",
	"+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", "##", "::", ".*",
	0
};

static int punct_len (const char *p)
{
	int i;
	for (i = 0; puncts[i]; i++) {
		int n = (int)strlen (puncts[i]);
		if (!strncmp (p, puncts[i], n))
			return n;
	}
	return ispunct ((unsigned char)*p) ? 1 : 0;
}

// a quoted literal starting at q (the quote); returns its end
static char *quoted_end (char *q, Token *at)
{
	char c = *q, *p = q + 1;
	for (; *p != c; p++) {
		if (*p == '\n' || *p == 0)
			error_at (at, "unterminated %s", c == '"' ? "string" : "character constant");
		if (*p == '\\')
			p++;
	}
	return p + 1;
}

static Token *new_token (int kind, char *start, char *end, File *f, int line)
{
	Token *t = xalloc (sizeof *t);
	t->kind = kind;
	t->loc = start;
	t->len = (int)(end - start);
	t->file = f;
	t->line = line;
	return t;
}

static Token *lex (char *p, File *f)
{
	Token head = {0}, *cur = &head;
	int line = 1, bol = 1, space = 0;
	while (*p) {
		Token *t;
		Token where;
		if (*p == '\n') {
			p++; line++;
			bol = 1; space = 0;
			continue;
		}
		if (*p == ' ' || *p == '\t' || *p == '\f' || *p == '\v') {
			p++;
			space = 1;
			continue;
		}
		if (p[0] == '/' && p[1] == '/') {
			while (*p && *p != '\n')
				p++;
			space = 1;
			continue;
		}
		if (p[0] == '/' && p[1] == '*') {
			char *e = strstr (p + 2, "*/");
			memset (&where, 0, sizeof where);
			where.file = f; where.line = line;
			if (!e)
				error_at (&where, "unterminated comment");
			for (; p < e; p++)
				if (*p == '\n')
					line++;
			p = e + 2;
			space = 1;
			continue;
		}
		memset (&where, 0, sizeof where);
		where.file = f; where.line = line;
		// string and character literals, with L, u, U, u8 prefixes
		{
			char *q = p;
			if (q[0] == 'u' && q[1] == '8')
				q += 2;
			else if (*q == 'L' || *q == 'u' || *q == 'U')
				q++;
			if (*q == '"' || *q == '\'') {
				char *e = quoted_end (q, &where);
				t = new_token (*q == '"' ? TK_STR : TK_CHAR, p, e, f, line);
				p = e;
				goto add;
			}
		}
		// preprocessing numbers
		if (isdigit ((unsigned char)*p) || (*p == '.' && isdigit ((unsigned char)p[1]))) {
			char *q = p++;
			for (;;) {
				if (p[0] && p[1] && strchr ("eEpP", p[0]) && strchr ("+-", p[1]))
					p += 2;
				else if (isalnum ((unsigned char)*p) || *p == '.' || *p == '_')
					p++;
				else
					break;
			}
			t = new_token (TK_NUM, q, p, f, line);
			goto add;
		}
		if (is_ident_start (*p)) {
			char *q = p;
			while (is_ident_char (*p))
				p++;
			t = new_token (TK_IDENT, q, p, f, line);
			goto add;
		}
		{
			int n = punct_len (p);
			if (!n)
				error_at (&where, "stray character 0x%02x", (unsigned char)*p);
			t = new_token (TK_PUNCT, p, p + n, f, line);
			p += n;
		}
	add:
		t->bol = (char)bol;
		t->space = (char)space;
		bol = space = 0;
		cur = cur->next = t;
	}
	cur->next = new_token (TK_EOF, p, p, f, line);
	cur->next->bol = 1;
	return head.next;
}

Token *tokenize (File *f)
{
	return lex (f->text, f);
}

Token *tokenize_string (char *text, File *as)
{
	File *f = new_file (as ? as->name : "<built-in>", text);
	f->index = as ? as->index : 0;
	return lex (f->text, f);
}

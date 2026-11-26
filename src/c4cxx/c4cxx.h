// c4cxx.h -- c4cxx, a C++-subset to C translator for c4 (docs/c4cxx-plan.md).
// M1: the preprocessor and lexer, a C parser and a C emitter, so that
// c4cxx's output compiles with c4lc to the same object as its input does.
//
// Plain C (C89 with // comments and declarations where they are used), so
// that gcc builds it on the host and c4lc can build it later.
#ifndef C4CXX_H
#define C4CXX_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>

typedef struct File File;
typedef struct Token Token;
typedef struct Hideset Hideset;

// ---- util.c ----
void *xalloc (size_t n);			// zeroed, or exit
char *xstrndup (const char *s, int n);
char *xformat (const char *fmt, ...);
void fatal (const char *fmt, ...);
void error_at (Token *t, const char *fmt, ...);
void warn_at (Token *t, const char *fmt, ...);
char *read_file (const char *path);		// 0 if it cannot be read

// a growable byte buffer
typedef struct { char *p; int n, cap; } Buf;
void buf_add (Buf *b, const char *s, int n);
void buf_str (Buf *b, const char *s);
void buf_char (Buf *b, int c);
void buf_fmt (Buf *b, const char *fmt, ...);

// ---- lex.c ----
enum {
	TK_IDENT,	// identifiers and keywords
	TK_PUNCT,	// punctuators
	TK_NUM,		// preprocessing numbers
	TK_CHAR,	// character constants, with any prefix
	TK_STR,		// string literals, with any prefix
	TK_PRAGMA,	// a whole #pragma line, passed through (text in loc/len)
	TK_EOF
};

struct File {
	char *name;	// as named (for __FILE__ and line markers)
	char *dir;	// its directory, with a trailing '/', or ""
	char *text;	// contents, line splices removed, LF line ends
	int index;
	int c_header;	// found in an -isystem directory: its declarations are C
};

struct Token {
	int kind;
	Token *next;
	char *loc;		// the spelling, in the file's text or made
	int len;
	File *file;
	int line;
	char bol;		// first on its line
	char space;		// preceded by white space
	Hideset *hideset;	// macros not to expand again (pp)
	Token *origin;		// a macro's expansion: the name it came from
};

Token *tokenize (File *f);
Token *tokenize_string (char *text, File *as);	// a pasted or made spelling
File *new_file (const char *name, char *text);
Token *copy_token (Token *t);
int equal (Token *t, const char *s);		// spelled s
int is_ident_start (int c);
int is_ident_char (int c);

// ---- pp.c ----
extern int opt_cplusplus;
void pp_add_include_path (const char *dir);
void pp_add_c_include_path (const char *dir);	// -isystem: headers of C
void pp_define (const char *spec);		// NAME or NAME=VALUE
void pp_undef (const char *name);
Token *preprocess (const char *path);		// the whole unit
void print_tokens (FILE *out, Token *t);	// -E
void pp_set_command_line (char **defs, int ndef, char **undefs, int nundef);

#endif

// cpp.c - a C preprocessor for the C4 toolchain.
//
//   ./cpp [-Idir]... [-DNAME[=VAL]]... file.c [file2.c...]   > out.i
//   ./c4 src/c4dos/cpp.c -Iinclude file.c                    (the purity pin)
//
// c4cc has no preprocessor at all - its lexer skips '#' lines - and the
// builds lean on `gcc -E`. c4lc's L9 preprocessor is real but is a Lisp
// program: far too slow under nested interpretation. This is the missing
// piece for the C4DOS self-hosting ladder (see docs/c4dos-design.md):
// a standalone pass, DOS-era style - CPP FOO.C > FOO.I, then C4CC FOO.I.
// Output goes to stdout, like gcc -E; multiple inputs concatenate, the
// same convention as c4cc's multi-file input.
//
// Features (scoped by a survey of everything under include/ and the
// kernels - nothing in the tree uses more):
//   #include "x" / <x>   quoted tries the including file's dir first,
//                        then -I dirs; angle skips the file's dir
//   #define NAME body    object-like
//   #define NAME(a,b) b  function-like: parameter substitution, then
//                        recursive rescan; self-reference guarded
//   #undef / #ifdef / #ifndef / #if / #elif / #else / #endif
//   #error / #pragma (ignored) / # NNN line markers (dropped)
//   -D NAME[=VAL]        predefines (build convention: -DC4CC=1
//                        -D__c4__=1 -D__C4CC__=1 -D__c4cc__=1)
//   #if expressions: integers, identifiers (expanded; undefined -> 0),
//   defined(X), ! ~ unary- * / % + - << >> < > <= >= == != & ^ | && ||
//
// NOT implemented, and refused loudly rather than mangled silently:
// ## paste, # stringize, backslash-newline continuations, variadic
// macros, #include_next. The tree does not use them.
//
// Comments are STRIPPED from macro bodies (a // in a body would eat the
// rest of every expansion site) and passed through verbatim everywhere
// else; expansion never fires inside strings, chars, or comments.
// Skipped conditional groups are scanned line-wise with comment
// tracking, so a '#endif' inside a comment does not close a group.
//
// The pin: for every corpus file,  cpp | c4cc  and  gcc -E -P | c4cc
// produce byte-identical .c4r images (src/c4dos/tests/test-cpp.sh).
//
// Written in the STRICT c4 subset (c4l.c's rules): no switch, no
// break/continue/for, no structs, locals at function top, single-word
// globals, functions defined before use (c4 is single-pass, so the
// include/expansion recursion is self-recursion only - directives are
// handled INLINE in process(), not in a helper that would need a
// forward reference). '\t' and '\r' are written as 9 and 13: the c4
// family lexers mis-translate the escapes (see docs/internals.md).

// Native build (for speed in build scripts, replacing gcc -E):
//   gcc -O2 -o cpp src/c4dos/cpp.c
// The includes and the int redefinition are the c4.c pattern: plain c4
// and c4cc both skip '#' lines, gcc needs the headers and a
// pointer-width int. Order matters - includes BEFORE the redefine.
#include <stdio.h>
#include <stdlib.h>
#include <memory.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#define int long long

// plain c4's enum parser takes literal numbers only - no expressions
enum { OUTCAP0   = 2097152 };   // output buffer, grows by doubling (2MB)
enum { FILEMAX   = 2097152 };   // one source file, 2MB
enum { MAXMAC    = 4096 };
enum { MAXPARAMS = 8 };
enum { MAXDIRS   = 8 };
enum { MAXCOND   = 64 };
enum { MAXDEPTH  = 32 };
enum { NAMEMAX   = 256 };
enum { ARGMAX    = 16384 };     // one macro argument's text
enum { BODYMAX   = 16384 };     // one substituted body

// conditional-stack states
enum { C_SKIP = 0, C_TAKE = 1, C_DONE = 2, C_DEAD = 3 };

char *g_out;   int g_outn; int g_outcap;
char **g_idirs; int g_nidirs;

char **m_name; int *m_nlen; char **m_body; int *m_np; char **m_par; int *m_busy;
int g_nmac;

int *c_stk; int g_ncond;

char *g_file;  // current file name, for diagnostics
int g_line;
int g_depth;   // include depth
int g_scratch; // shared read buffer (char*), reused per file read

int isids (int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
int isidc (int c) { return isids(c) || (c >= '0' && c <= '9'); }
int isws  (int c) { return c == ' ' || c == 9; }

int xlen (char *s) { int n; n = 0; while (s[n]) ++n; return n; }

int die (char *msg, char *what) {
  printf("cpp: %s:%d: %s%s\n", g_file, g_line, msg, what ? what : "");
  exit(1);
  return 0;
}

int emit (int c) {
  char *nb; int i;
  if (g_outn + 1 >= g_outcap) {
    g_outcap = g_outcap * 2;
    if (!(nb = malloc(g_outcap))) die("out of memory growing output", 0);
    i = 0;
    while (i < g_outn) { nb[i] = g_out[i]; ++i; }
    free(g_out);
    g_out = nb;
  }
  g_out[g_outn++] = c;
  return 0;
}

int emitn (char *p, int n) { int i; i = 0; while (i < n) emit(p[i++]); return 0; }

// --- macro table -----------------------------------------------------------

int mac_find (char *p, int n) {
  int i;
  i = 0;
  while (i < g_nmac) {
    if (m_nlen[i] == n && m_name[i] && !memcmp(m_name[i], p, n)) return i;
    ++i;
  }
  return -1;
}

char *strdupn (char *p, int n) {
  char *s; int i;
  if (!(s = malloc(n + 1))) die("out of memory", 0);
  i = 0;
  while (i < n) { s[i] = p[i]; ++i; }
  s[n] = 0;
  return s;
}

// params packed as "a\0b\0c\0", np entries
int mac_add (char *name, int nlen, char *params, int np, char *body) {
  int i;
  i = mac_find(name, nlen);
  if (i >= 0) { m_name[i] = 0; }   // redefinition: last one wins
  if (g_nmac >= MAXMAC) die("too many macros", 0);
  i = g_nmac++;
  m_name[i] = strdupn(name, nlen);
  m_nlen[i] = nlen;
  m_par[i] = params;
  m_np[i] = np;
  m_body[i] = body;
  m_busy[i] = 0;
  return i;
}

// --- file reading ----------------------------------------------------------

// read whole file into an exact-size NUL-terminated allocation, or 0
char *read_file (char *path) {
  int fd, n, total; char *s;
  if ((fd = open(path, 0)) < 0) return 0;
  total = 0;
  while ((n = read(fd, (char *)g_scratch + total, 65536)) > 0) {
    total = total + n;
    if (total + 65536 >= FILEMAX) { close(fd); die("file too large: ", path); }
  }
  close(fd);
  s = strdupn((char *)g_scratch, total);
  return s;
}

// "dir/name" -> malloc'd joined path; dir may be "" (then just name)
char *joinpath (char *dir, char *name) {
  int dn, nn, i; char *s;
  dn = xlen(dir);
  nn = xlen(name);
  if (!(s = malloc(dn + nn + 2))) die("out of memory", 0);
  i = 0;
  while (i < dn) { s[i] = dir[i]; ++i; }
  if (dn) s[i++] = '/';
  nn = 0;
  while (name[nn]) { s[i++] = name[nn++]; }
  s[i] = 0;
  return s;
}

// quoted includes try the including file's directory first
char *resolve_include (char *name, int quoted, char *curdir) {
  char *buf, *path; int i;
  if (quoted && curdir) {
    path = joinpath(curdir, name);
    buf = read_file(path);
    free(path);
    if (buf) return buf;
  }
  i = 0;
  while (i < g_nidirs) {
    path = joinpath(g_idirs[i], name);
    buf = read_file(path);
    free(path);
    if (buf) return buf;
    ++i;
  }
  return read_file(name);
}

// --- #if expression evaluation --------------------------------------------
//
// Precedence-climbing over the directive text. Identifiers expand
// through the macro table (object-like only; recursion via the same
// evaluator); undefined identifiers evaluate to 0, defined(X) is
// handled before lookup. e_pos/e_end are globals so the single
// function can recurse (plain c4: no mutual recursion).

char *e_pos; char *e_end;

// skip whitespace AND comments - '#if NATIVE // note' is legal input
int e_ws () {
  int go;
  go = 1;
  while (go) {
    go = 0;
    while (e_pos < e_end && isws(*e_pos)) ++e_pos;
    if (e_pos + 1 < e_end && *e_pos == '/' && e_pos[1] == '/') e_pos = e_end;
    else if (e_pos + 1 < e_end && *e_pos == '/' && e_pos[1] == '*') {
      e_pos = e_pos + 2;
      while (e_pos + 1 < e_end && !(*e_pos == '*' && e_pos[1] == '/')) ++e_pos;
      if (e_pos + 1 < e_end) e_pos = e_pos + 2; else e_pos = e_end;
      go = 1;
    }
  }
  return 0;
}

int prec_of (int op) {
  // two-char ops encoded as first*256+second
  if (op == '|' * 256 + '|') return 1;
  if (op == '&' * 256 + '&') return 2;
  if (op == '|') return 3;
  if (op == '^') return 4;
  if (op == '&') return 5;
  if (op == '=' * 256 + '=') return 6;
  if (op == '!' * 256 + '=') return 6;
  if (op == '<') return 7;
  if (op == '>') return 7;
  if (op == '<' * 256 + '=') return 7;
  if (op == '>' * 256 + '=') return 7;
  if (op == '<' * 256 + '<') return 8;
  if (op == '>' * 256 + '>') return 8;
  if (op == '+') return 9;
  if (op == '-') return 9;
  if (op == '*') return 10;
  if (op == '/') return 10;
  if (op == '%') return 10;
  return 0;
}

int eval_expr (int minprec) {
  int v, r, op, neg, mi, n; char *p; char *sp; char *se;
  e_ws();
  if (e_pos >= e_end) die("#if: expression expected", 0);

  // primary (with unary operators, self-recursive)
  if (*e_pos == '(') {
    ++e_pos;
    v = eval_expr(1);
    e_ws();
    if (e_pos < e_end && *e_pos == ')') ++e_pos; else die("#if: missing )", 0);
  }
  else if (*e_pos == '!') { ++e_pos; v = !eval_expr(11); }
  else if (*e_pos == '~') { ++e_pos; v = ~eval_expr(11); }
  else if (*e_pos == '-') { ++e_pos; v = -eval_expr(11); }
  else if (*e_pos == '+') { ++e_pos; v = eval_expr(11); }
  else if (*e_pos >= '0' && *e_pos <= '9') {
    v = 0;
    if (*e_pos == '0' && e_pos + 1 < e_end && (e_pos[1] == 'x' || e_pos[1] == 'X')) {
      e_pos = e_pos + 2;
      while (e_pos < e_end &&
             ((*e_pos >= '0' && *e_pos <= '9') || (*e_pos >= 'a' && *e_pos <= 'f')
              || (*e_pos >= 'A' && *e_pos <= 'F'))) {
        n = *e_pos;
        if (n >= 'a') n = n - 'a' + 10;
        else if (n >= 'A') n = n - 'A' + 10;
        else n = n - '0';
        v = v * 16 + n;
        ++e_pos;
      }
    } else {
      while (e_pos < e_end && *e_pos >= '0' && *e_pos <= '9') v = v * 10 + *e_pos++ - '0';
    }
    // swallow integer suffixes (1L, 0x10UL...)
    while (e_pos < e_end && (*e_pos == 'l' || *e_pos == 'L' || *e_pos == 'u' || *e_pos == 'U')) ++e_pos;
  }
  else if (*e_pos == '\'') {
    ++e_pos;
    v = *e_pos++;
    if (v == '\\') {
      v = *e_pos++;
      if (v == 'n') v = 10;
      else if (v == 't') v = 9;
      else if (v == 'r') v = 13;
      else if (v == '0') v = 0;
    }
    if (e_pos < e_end && *e_pos == '\'') ++e_pos;
  }
  else if (isids(*e_pos)) {
    p = e_pos;
    while (e_pos < e_end && isidc(*e_pos)) ++e_pos;
    n = e_pos - p;
    if (n == 7 && !memcmp(p, "defined", 7)) {
      e_ws();
      neg = 0;
      if (e_pos < e_end && *e_pos == '(') { neg = 1; ++e_pos; e_ws(); }
      p = e_pos;
      while (e_pos < e_end && isidc(*e_pos)) ++e_pos;
      v = mac_find(p, e_pos - p) >= 0;
      e_ws();
      if (neg) { if (e_pos < e_end && *e_pos == ')') ++e_pos; else die("#if: defined( without )", 0); }
    } else {
      mi = mac_find(p, n);
      if (mi >= 0 && !m_busy[mi] && m_np[mi] < 0) {
        // evaluate the macro's body as an expression (save our cursor)
        sp = e_pos; se = e_end;
        e_pos = m_body[mi];
        e_end = m_body[mi] + xlen(m_body[mi]);
        m_busy[mi] = 1;
        v = e_pos < e_end ? eval_expr(1) : 0;
        m_busy[mi] = 0;
        e_pos = sp; e_end = se;
      } else {
        v = 0;   // undefined identifier: standard says 0
      }
    }
  }
  else die("#if: cannot parse expression", 0);

  // binary operators by precedence climbing
  while (1) {
    e_ws();
    if (e_pos >= e_end) return v;
    op = *e_pos;
    if (op == ')' || op == ',') return v;
    if (e_pos + 1 < e_end &&
        ((op == '&' && e_pos[1] == '&') || (op == '|' && e_pos[1] == '|')
         || (op == '=' && e_pos[1] == '=') || (op == '!' && e_pos[1] == '=')
         || (op == '<' && e_pos[1] == '=') || (op == '>' && e_pos[1] == '=')
         || (op == '<' && e_pos[1] == '<') || (op == '>' && e_pos[1] == '>'))) {
      op = op * 256 + e_pos[1];
    }
    if (!prec_of(op) || prec_of(op) < minprec) return v;
    e_pos = e_pos + (op > 255 ? 2 : 1);
    r = eval_expr(prec_of(op) + 1);
    if (op == '|' * 256 + '|') v = v || r;
    else if (op == '&' * 256 + '&') v = v && r;
    else if (op == '|') v = v | r;
    else if (op == '^') v = v ^ r;
    else if (op == '&') v = v & r;
    else if (op == '=' * 256 + '=') v = v == r;
    else if (op == '!' * 256 + '=') v = v != r;
    else if (op == '<' * 256 + '=') v = v <= r;
    else if (op == '>' * 256 + '=') v = v >= r;
    else if (op == '<' * 256 + '<') v = v << r;
    else if (op == '>' * 256 + '>') v = v >> r;
    else if (op == '<') v = v < r;
    else if (op == '>') v = v > r;
    else if (op == '+') v = v + r;
    else if (op == '-') v = v - r;
    else if (op == '*') v = v * r;
    else if (op == '/') { if (!r) die("#if: division by zero", 0); v = v / r; }
    else if (op == '%') { if (!r) die("#if: division by zero", 0); v = v % r; }
  }
  return v;
}

// --- expansion -------------------------------------------------------------
//
// expand_text copies [p, end) to the output, tracking strings, chars
// and comments, expanding macros at identifiers. Function-like macro
// arguments are captured RAW and substituted textually into the body;
// the substituted body is then rescanned by self-recursion with the
// macro's busy flag set. Newlines inside an argument list are
// swallowed (they belong to the call, as gcc does).

int expand_text (char *p, char *end) {
  char *q, *body, *sub, *arg; char *pstart;
  int n, mi, i, k, depth, nargs, quoted, plen, alen, found;
  char **argv2; int *argl;

  while (p < end) {
    // (plain c4 has no `continue`, so this is one else-if cascade)
    if (*p == '/' && p + 1 < end && p[1] == '/') {
      // line comment: verbatim
      while (p < end && *p != 10) emit(*p++);
    }
    else if (*p == '/' && p + 1 < end && p[1] == '*') {
      // block comment: verbatim
      emit(*p++); emit(*p++);
      while (p < end && !(*p == '*' && p + 1 < end && p[1] == '/')) emit(*p++);
      if (p < end) { emit(*p++); emit(*p++); }
    }
    else if (*p == '"' || *p == '\'') {
      // string and char literals: verbatim, escapes respected
      quoted = *p;
      emit(*p++);
      while (p < end && *p != quoted) {
        if (*p == '\\' && p + 1 < end) emit(*p++);
        emit(*p++);
      }
      if (p < end) emit(*p++);
    }
    else if (*p >= '0' && *p <= '9') {
      // numbers: swallow whole so "0x1f" never has its 'f' expanded
      while (p < end && (isidc(*p) || *p == '.')) emit(*p++);
    }
    else if (!isids(*p)) emit(*p++);
    else if (1) {
      // identifier
      q = p;
      while (p < end && isidc(*p)) ++p;
      n = p - q;
      mi = mac_find(q, n);
      if (mi < 0 || m_busy[mi]) emitn(q, n);
      else if (m_np[mi] < 0) {
        // object-like: rescan the body
        m_busy[mi] = 1;
        expand_text(m_body[mi], m_body[mi] + xlen(m_body[mi]));
        m_busy[mi] = 0;
      }
      else if (1) {
        // function-like: needs a '(' (whitespace allowed); else plain name
        q = p;
        while (q < end && (isws(*q) || *q == 10 || *q == 13)) ++q;
        if (q >= end || *q != '(') emitn(p - n, n);
        else {
          // a call: capture arguments, substitute, rescan (inline -
          // plain c4 is single-pass, so no helper may call back into
          // expand_text; self-recursion is the only recursion)
          p = q + 1;
          if (!(argv2 = malloc(MAXPARAMS * sizeof(char *)))) die("out of memory", 0);
    if (!(argl = malloc(MAXPARAMS * sizeof(int)))) die("out of memory", 0);
    if (!(arg = malloc(ARGMAX))) die("out of memory", 0);
    nargs = 0; alen = 0; depth = 0; found = 0;
    while (p < end && !found) {
      if (depth == 0 && (*p == ')' || *p == ',')) {
        if (nargs >= MAXPARAMS) die("too many macro arguments", 0);
        // trim
        i = 0;
        while (i < alen && (isws(arg[i]) || arg[i] == 10 || arg[i] == 13)) ++i;
        k = alen;
        while (k > i && (isws(arg[k - 1]) || arg[k - 1] == 10 || arg[k - 1] == 13)) --k;
        argv2[nargs] = strdupn(arg + i, k - i);
        argl[nargs] = k - i;
        ++nargs;
        alen = 0;
        if (*p == ')') found = 1;
        ++p;
      } else {
        if (*p == '(') ++depth;
        if (*p == ')') --depth;
        if (*p == '"' || *p == '\'') {
          quoted = *p;
          if (alen < ARGMAX - 1) arg[alen++] = *p;
          ++p;
          while (p < end && *p != quoted) {
            if (*p == '\\' && p + 1 < end) { if (alen < ARGMAX - 1) arg[alen++] = *p; ++p; }
            if (alen < ARGMAX - 1) arg[alen++] = *p;
            ++p;
          }
        }
        if (p < end) {
          if (alen >= ARGMAX - 1) die("macro argument too long", 0);
          arg[alen++] = *p++;
        }
      }
    }
    if (!found) die("unterminated macro call: ", m_name[mi]);
    if (nargs == 1 && argl[0] == 0 && m_np[mi] == 0) nargs = 0;  // NAME()
    if (nargs != m_np[mi]) die("wrong number of arguments to macro: ", m_name[mi]);

    // substitute params into the body
    if (!(sub = malloc(BODYMAX))) die("out of memory", 0);
    k = 0;
    body = m_body[mi];
    while (*body) {
      if (*body == '"' || *body == '\'') {
        quoted = *body;
        if (k < BODYMAX - 1) sub[k++] = *body;
        ++body;
        while (*body && *body != quoted) {
          if (*body == '\\' && body[1]) { if (k < BODYMAX - 1) sub[k++] = *body; ++body; }
          if (k < BODYMAX - 1) sub[k++] = *body;
          ++body;
        }
        if (*body) { if (k < BODYMAX - 1) sub[k++] = *body; ++body; }
      }
      else if (isids(*body)) {
        pstart = body;
        while (isidc(*body)) ++body;
        plen = body - pstart;
        // param lookup: packed "a\0b\0..."
        q = m_par[mi];
        i = 0; found = 0;
        while (i < m_np[mi] && !found) {
          if (xlen(q) == plen && !memcmp(q, pstart, plen)) found = 1;
          else { q = q + xlen(q) + 1; ++i; }
        }
        if (found) {
          n = 0;
          while (n < argl[i]) {
            if (k >= BODYMAX - 1) die("macro expansion too long", 0);
            sub[k++] = argv2[i][n++];
          }
        } else {
          n = 0;
          while (n < plen) {
            if (k >= BODYMAX - 1) die("macro expansion too long", 0);
            sub[k++] = pstart[n++];
          }
        }
      }
      else {
        if (k >= BODYMAX - 1) die("macro expansion too long", 0);
        sub[k++] = *body++;
      }
    }
    sub[k] = 0;

    // rescan
    m_busy[mi] = 1;
    expand_text(sub, sub + k);
    m_busy[mi] = 0;

          free(sub);
          i = 0;
          while (i < nargs) free(argv2[i++]);
          free(argv2); free(argl); free(arg);
        }
      }
    }
  }
  return 0;
}

// --- the driver ------------------------------------------------------------
//
// process() owns everything that needs the include recursion: the
// line walk, directive handling (INLINE - plain c4 is single-pass and
// a do_directive helper would need a forward reference to recurse for
// #include), conditional stack, and handing code stretches to
// expand_text. `dir` is the directory of THIS file, for quoted
// includes; both are restored around recursion.

int taking () { return g_ncond == 0 || c_stk[g_ncond - 1] == C_TAKE; }

int process (char *buf, char *fname, char *dir) {
  char *p, *q, *e, *name, *body, *params, *savefile, *sub, *incbuf, *incdir;
  int savedline, n, i, k, np, quoted, state, cbase, incomment;

  savefile = g_file; savedline = g_line;
  g_file = fname; g_line = 1;
  if (++g_depth > MAXDEPTH) die("includes nested too deeply", 0);
  cbase = g_ncond;   // a file must balance its own conditionals
  incomment = 0;

  p = buf;
  while (*p) {
    // ---- find this line's extent ----
    e = p;
    while (*e && *e != 10) ++e;   // e -> newline or NUL

    // lookahead for the directive test (before the cascade: plain c4
    // has no `continue`, so each branch below falls to the loop end)
    q = p;
    while (q < e && isws(*q)) ++q;

    // ---- inside a block comment: verbatim until it closes ----
    if (incomment) {
      q = p;
      while (q < e && !(*q == '*' && q[1] == '/')) ++q;
      if (q < e) {
        incomment = 0;
        if (taking()) emitn(p, q + 2 - p);
        p = q + 2;        // rest of the line goes around again
      } else {
        if (taking()) { emitn(p, e - p); if (*e) emit(10); }
        p = *e ? e + 1 : e;
        ++g_line;
      }
    }

    // ---- directive? ----
    else if (q < e && *q == '#') {
      ++q;
      while (q < e && isws(*q)) ++q;
      name = q;
      while (q < e && isids(*q)) ++q;
      n = q - name;
      while (q < e && isws(*q)) ++q;
      // continuations are not supported: refuse rather than mangle
      if (e > p && e[-1] == '\\') die("backslash-newline continuation is not supported", 0);

      if (n == 5 && !memcmp(name, "ifdef", 5)) {
        if (g_ncond >= MAXCOND) die("conditionals nested too deeply", 0);
        if (!taking()) c_stk[g_ncond++] = C_DEAD;
        else {
          name = q;
          while (q < e && isidc(*q)) ++q;
          c_stk[g_ncond++] = mac_find(name, q - name) >= 0 ? C_TAKE : C_SKIP;
        }
      }
      else if (n == 6 && !memcmp(name, "ifndef", 6)) {
        if (g_ncond >= MAXCOND) die("conditionals nested too deeply", 0);
        if (!taking()) c_stk[g_ncond++] = C_DEAD;
        else {
          name = q;
          while (q < e && isidc(*q)) ++q;
          c_stk[g_ncond++] = mac_find(name, q - name) >= 0 ? C_SKIP : C_TAKE;
        }
      }
      else if (n == 2 && !memcmp(name, "if", 2)) {
        if (g_ncond >= MAXCOND) die("conditionals nested too deeply", 0);
        if (!taking()) c_stk[g_ncond++] = C_DEAD;
        else {
          e_pos = q; e_end = e;
          c_stk[g_ncond++] = eval_expr(1) ? C_TAKE : C_SKIP;
        }
      }
      else if (n == 4 && !memcmp(name, "elif", 4)) {
        if (g_ncond <= cbase) die("#elif without #if", 0);
        state = c_stk[g_ncond - 1];
        if (state == C_TAKE) c_stk[g_ncond - 1] = C_DONE;
        else if (state == C_SKIP) {
          e_pos = q; e_end = e;
          if (eval_expr(1)) c_stk[g_ncond - 1] = C_TAKE;
        }
        // C_DONE and C_DEAD stay put
      }
      else if (n == 4 && !memcmp(name, "else", 4)) {
        if (g_ncond <= cbase) die("#else without #if", 0);
        state = c_stk[g_ncond - 1];
        if (state == C_TAKE) c_stk[g_ncond - 1] = C_DONE;
        else if (state == C_SKIP) c_stk[g_ncond - 1] = C_TAKE;
      }
      else if (n == 5 && !memcmp(name, "endif", 5)) {
        if (g_ncond <= cbase) die("#endif without #if", 0);
        --g_ncond;
      }
      else if (!taking()) {
        // any other directive in a skipped group: ignore entirely
      }
      else if (n == 7 && !memcmp(name, "include", 7)) {
        if (q >= e || (*q != '"' && *q != '<')) die("#include expects \"file\" or <file>", 0);
        quoted = *q == '"';
        ++q;
        name = q;
        while (q < e && *q != '"' && *q != '>') ++q;
        name = strdupn(name, q - name);
        if (!(incbuf = resolve_include(name, quoted, dir))) die("cannot find include file: ", name);
        // directory of the included file, for ITS quoted includes
        incdir = 0;
        i = xlen(name);
        while (i > 0 && name[i - 1] != '/') --i;
        if (i > 0) {
          // includes below a -I dir keep their resolved subdirectory
          // only for the path we actually opened; a plain name keeps
          // this file's dir. Close enough for this tree: quoted
          // includes here are all flat or repo-relative.
          incdir = strdupn(name, i - 1);
        } else incdir = dir;
        process(incbuf, name, incdir);
        free(incbuf);
      }
      else if (n == 6 && !memcmp(name, "define", 6)) {
        name = q;
        while (q < e && isidc(*q)) ++q;
        n = q - name;
        if (!n) die("#define expects a name", 0);
        np = -1;
        params = 0;
        if (q < e && *q == '(') {        // function-like: '(' with NO space
          ++q;
          if (!(params = malloc(NAMEMAX))) die("out of memory", 0);
          k = 0; np = 0;
          while (q < e && *q != ')') {
            while (q < e && (isws(*q) || *q == ',')) ++q;
            if (q < e && *q != ')') {
              if (np >= MAXPARAMS) die("too many macro parameters", 0);
              while (q < e && isidc(*q)) {
                if (k >= NAMEMAX - 2) die("macro parameter list too long", 0);
                params[k++] = *q++;
              }
              params[k++] = 0;
              ++np;
            }
          }
          if (q >= e) die("#define: missing )", 0);
          ++q;
        }
        while (q < e && isws(*q)) ++q;
        // body: rest of line with comments stripped (a // in a body
        // would comment out the rest of every expansion site)
        if (!(body = malloc(e - q + 1))) die("out of memory", 0);
        k = 0;
        while (q < e) {
          if (*q == '/' && q + 1 < e && q[1] == '/') q = e;
          else if (*q == '/' && q + 1 < e && q[1] == '*') {
            q = q + 2;
            while (q + 1 < e && !(*q == '*' && q[1] == '/')) ++q;
            if (q + 1 < e) q = q + 2;
            else die("unterminated comment in macro body", 0);
            body[k++] = ' ';
          }
          else if (*q == '"' || *q == '\'') {
            quoted = *q;
            body[k++] = *q++;
            while (q < e && *q != quoted) {
              if (*q == '\\' && q + 1 < e) body[k++] = *q++;
              body[k++] = *q++;
            }
            if (q < e) body[k++] = *q++;
          }
          else body[k++] = *q++;
        }
        while (k > 0 && isws(body[k - 1])) --k;
        body[k] = 0;
        // refuse the operators we do not implement
        i = 0;
        while (body[i]) {
          if (body[i] == '#') die("## / # operators are not supported", 0);
          ++i;
        }
        mac_add(name, n, params, np, body);
      }
      else if (n == 5 && !memcmp(name, "undef", 5)) {
        name = q;
        while (q < e && isidc(*q)) ++q;
        i = mac_find(name, q - name);
        if (i >= 0) m_name[i] = 0;
      }
      else if (n == 5 && !memcmp(name, "error", 5)) {
        die("#error: ", strdupn(q, e - q));
      }
      else if (n == 6 && !memcmp(name, "pragma", 6)) {
        // passed through verbatim, as gcc -E does; c4cc skips '#' lines
        emitn(p, e - p);
        emit(10);
      }
      else if (n == 12 && !memcmp(name, "include_next", 12)) {
        die("#include_next is not supported", 0);
      }
      else if (n == 0 && (q >= e || (*q >= '0' && *q <= '9'))) {
        // bare '#' or a '# NNN' line marker: drop
      }
      else {
        die("unknown directive: ", strdupn(name, n));
      }
      p = *e ? e + 1 : e;
      ++g_line;
    }

    // ---- ordinary line, skipped group ----
    else if (!taking()) {
      // only comment state matters (so a '#endif' inside a comment
      // cannot close the group)
      q = p;
      while (q < e) {
        if (*q == '/' && q + 1 < e && q[1] == '*') { incomment = 1; q = q + 2; }
        else if (incomment && *q == '*' && q + 1 < e && q[1] == '/') { incomment = 0; q = q + 2; }
        else if (!incomment && *q == '/' && q + 1 < e && q[1] == '/') q = e;
        else ++q;
      }
      p = *e ? e + 1 : e;
      ++g_line;
    }

    // ---- taken code ----
    // expand_text handles strings and both comment styles; a block
    // comment that does not close on this line flips `incomment` so
    // the next iterations pass it through verbatim.
    else {
     q = p;
     while (q < e) {
      if (*q == '/' && q + 1 < e && q[1] == '*') {
        k = 0;
        q = q + 2;
        while (q < e && !k) {
          if (*q == '*' && q + 1 < e && q[1] == '/') { k = 1; q = q + 2; }
          else ++q;
        }
        if (!k) incomment = 1;
      }
      else if (*q == '/' && q + 1 < e && q[1] == '/') q = e;
      else if (*q == '"' || *q == '\'') {
        quoted = *q;
        ++q;
        while (q < e && *q != quoted) {
          if (*q == '\\' && q + 1 < e) ++q;
          ++q;
        }
        if (q < e) ++q;
      }
      else ++q;
     }
     expand_text(p, e);
     if (*e) emit(10);
     p = *e ? e + 1 : e;
     ++g_line;
    }
  }

  if (g_ncond != cbase) die("unterminated #if at end of file", 0);
  --g_depth;
  g_file = savefile; g_line = savedline;
  return 0;
}

// --- main ------------------------------------------------------------------

// Re-apply the -D predefines on a fresh macro table. Each input file
// is its own TRANSLATION UNIT, exactly like `gcc -E a.h b.c`: macro
// state (and thus include guards) resets between files, so a header
// passed on the command line AND included by a later file expands
// twice. That is what the shipping pipeline produces, c4cc absorbs it
// by design ("last definition wins" - README), and image parity with
// the gcc path depends on mimicking it.
char **d_spec; int g_ndefs;

int reset_tu () {
  char *name; int j, n2;
  g_nmac = 0;
  j = 0;
  while (j < g_ndefs) {
    name = d_spec[j];
    n2 = 0;
    while (name[n2] && name[n2] != '=') ++n2;
    mac_add(name, n2, 0, -1, name[n2] ? name + n2 + 1 : "1");
    ++j;
  }
  return 0;
}

int main (int argc, char **argv) {
  char *a, *buf, *dir, *name; int i, n, nfiles;

  g_outcap = OUTCAP0;
  if (!(g_out = malloc(g_outcap))) { printf("cpp: out of memory\n"); return 1; }
  g_outn = 0;
  if (!(g_scratch = (int)malloc(FILEMAX))) { printf("cpp: out of memory\n"); return 1; }
  if (!(g_idirs = malloc(MAXDIRS * sizeof(char *)))) { printf("cpp: out of memory\n"); return 1; }
  g_nidirs = 0;
  if (!(m_name = malloc(MAXMAC * sizeof(char *)))) { printf("cpp: out of memory\n"); return 1; }
  if (!(m_nlen = malloc(MAXMAC * sizeof(int)))) { printf("cpp: out of memory\n"); return 1; }
  if (!(m_body = malloc(MAXMAC * sizeof(char *)))) { printf("cpp: out of memory\n"); return 1; }
  if (!(m_np = malloc(MAXMAC * sizeof(int)))) { printf("cpp: out of memory\n"); return 1; }
  if (!(m_par = malloc(MAXMAC * sizeof(char *)))) { printf("cpp: out of memory\n"); return 1; }
  if (!(m_busy = malloc(MAXMAC * sizeof(int)))) { printf("cpp: out of memory\n"); return 1; }
  g_nmac = 0;
  if (!(c_stk = malloc(MAXCOND * sizeof(int)))) { printf("cpp: out of memory\n"); return 1; }
  g_ncond = 0;
  if (!(d_spec = malloc(MAXDIRS * 4 * sizeof(char *)))) { printf("cpp: out of memory\n"); return 1; }
  g_ndefs = 0;
  g_depth = 0;
  g_file = "<cmdline>";
  g_line = 0;

  --argc; ++argv;
  nfiles = 0;
  i = 0;
  while (i < argc) {
    a = argv[i];
    if (a[0] == '-' && a[1] == 'I') {
      if (a[2]) { if (g_nidirs < MAXDIRS) g_idirs[g_nidirs++] = a + 2; }
      else if (i + 1 < argc) { ++i; if (g_nidirs < MAXDIRS) g_idirs[g_nidirs++] = argv[i]; }
    }
    else if (a[0] == '-' && a[1] == 'D') {
      name = a[2] ? a + 2 : (i + 1 < argc ? argv[++i] : 0);
      if (name && g_ndefs < MAXDIRS * 4) d_spec[g_ndefs++] = name;
    }
    else if (a[0] == '-' && !a[1]) {
      // '-' (stdin) is a c4cc convention, not needed here yet
      printf("cpp: reading stdin is not supported\n");
      return 1;
    }
    else ++nfiles;   // an input file; second pass below
    ++i;
  }
  if (!nfiles) {
    printf("usage: cpp [-Idir]... [-DNAME[=VAL]]... file.c [file2.c...]\n");
    return 1;
  }

  // one TRANSLATION UNIT per input file (see reset_tu above)
  i = 0;
  while (i < argc) {
    a = argv[i];
    if (a[0] == '-' && (a[1] == 'I' || a[1] == 'D') && !a[2]) ++i;   // skip the flag's value
    else if (a[0] != '-') {
      if (!(buf = read_file(a))) { printf("cpp: cannot open %s\n", a); return 1; }
      // this file's directory, for its quoted includes
      n = xlen(a);
      while (n > 0 && a[n - 1] != '/') --n;
      dir = n > 0 ? strdupn(a, n - 1) : 0;
      reset_tu();
      process(buf, a, dir);
      free(buf);
    }
    ++i;
  }

  emit(0);
  printf("%s", g_out);
  return 0;
}

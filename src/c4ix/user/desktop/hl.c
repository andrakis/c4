// hl.c -- the IDE's syntax colouring (edit.c): what each character of a
// line is, for C and JavaScript, Lisp, Forth, shell scripts and Makefiles,
// and B4KE build files. A module of its own since edit.c grew past what
// C4IX's compiler can hold at once (docs/c4ix-desktop.md, part twenty).
//
// Each line is coloured on its own, character by character, into hlc;
// the one thing carried from line to line is being inside a C comment.

#include "desktop.h"

static int hlc[HL_MAX];

static int hl_isword(int c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

// The language a file's name says it is
int hl_lang(char *p) {
    if (ui_ends(p, ".c") || ui_ends(p, ".h")) return L_C;
    if (ui_ends(p, ".js") || ui_ends(p, ".mjs")) return L_JS;
    if (ui_ends(p, ".lisp") || ui_ends(p, ".scm") || ui_ends(p, ".el")) return L_LISP;
    if (ui_ends(p, ".f") || ui_ends(p, ".fs") || ui_ends(p, ".fth") || ui_ends(p, ".4th")) return L_FORTH;
    if (ui_ends(p, ".sh") || ui_ends(p, "Makefile") || ui_ends(p, ".mk")) return L_SH;
    if (ui_ends(p, ".b4k") || ui_ends(p, ".B4K")) return L_B4K;
    return L_TEXT;
}

// Is S[0..N) one of LIST's words? LIST has a space before each word and
// after the last.
int hl_inwords(char *s, int n, char *list) {
    int i, k;
    i = 0;
    while (list[i]) {
        if (list[i] == ' ' && list[i + 1]) {
            k = 0;
            while (k < n && list[i + 1 + k] == s[k]) ++k;
            if (k == n && list[i + 1 + k] == ' ') return 1;
        }
        ++i;
    }
    return 0;
}

static void hl_paint(int i, int j, int c) {
    while (i < j && i < HL_MAX) { hlc[i] = c; ++i; }
}

static char *c_kw = " if else while for do switch case default break continue return goto sizeof typedef struct union enum static extern const volatile register inline restrict ";
static char *c_ty = " int char short long unsigned signed void float double ";
static char *js_kw = " function var let const if else for while do return new class this null true false undefined import export from of in typeof instanceof await async break continue switch case default throw try catch finally delete yield ";
static char *lisp_kw = " define lambda if begin let let* letrec set! cond else and or not quote next define-macro when unless do case ";
static char *forth_kw = " IF ELSE THEN BEGIN WHILE REPEAT UNTIL AGAIN DO ?DO LOOP +LOOP LEAVE UNLOOP EXIT ; : VARIABLE CONSTANT VALUE CREATE DOES> RECURSE DEFER IS TO CASE OF ENDOF ENDCASE IMMEDIATE POSTPONE ['] [CHAR] LITERAL :NONAME {: :} :M ;M ";
static char *sh_kw = " if then else elif fi for in do done while until case esac function return export local set exit break continue shift ";

// a string from I (its quote) to its end, \ escapes skipped
static int hl_strend(char *s, int n, int i) {
    int q;
    q = s[i];
    ++i;
    while (i < n && s[i] != q) { if (s[i] == 92) ++i; ++i; }
    return i < n ? i + 1 : n;
}

// C and JavaScript; ST 1 is inside /* */
static int hl_c(char *s, int n, int st, int js) {
    int i, j, c, nb;
    i = 0; nb = 0;
    while (i < n) {
        c = s[i];
        if (st == 1) {
            j = i;
            while (j < n && !(s[j] == '*' && j + 1 < n && s[j + 1] == '/')) ++j;
            if (j < n) { j = j + 2; st = 0; }
            hl_paint(i, j, HC_COM); i = j;
        } else if (c == '/' && i + 1 < n && s[i + 1] == '/') { hl_paint(i, n, HC_COM); i = n; }
        else if (c == '/' && i + 1 < n && s[i + 1] == '*') { hl_paint(i, i + 2, HC_COM); i = i + 2; st = 1; }
        else if (c == '"' || c == 39 || (js && c == '`')) { j = hl_strend(s, n, i); hl_paint(i, j, HC_STR); i = j; }
        else if (!js && c == '#' && !nb) {
            j = i + 1;
            while (j < n && s[j] == ' ') ++j;
            while (j < n && hl_isword(s[j])) ++j;
            hl_paint(i, j, HC_PRE); i = j;
        } else if (c >= '0' && c <= '9' && (i == 0 || !hl_isword(s[i - 1]))) {
            j = i;
            while (j < n && (hl_isword(s[j]) || s[j] == '.')) ++j;
            hl_paint(i, j, HC_NUM); i = j;
        } else if (hl_isword(c)) {
            j = i;
            while (j < n && hl_isword(s[j])) ++j;
            if (hl_inwords(s + i, j - i, js ? js_kw : c_kw)) hl_paint(i, j, HC_KW);
            else if (!js && hl_inwords(s + i, j - i, c_ty)) hl_paint(i, j, HC_TYPE);
            i = j;
        } else ++i;
        if (c != ' ' && c != 9) nb = 1;
    }
    return st;
}

static int hl_lispword(int c) { return c > ' ' && c != '(' && c != ')' && c != '"' && c != ';' && c != 39; }
static void hl_lisp(char *s, int n) {
    int i, j, c;
    i = 0;
    while (i < n) {
        c = s[i];
        if (c == ';') { hl_paint(i, n, HC_COM); i = n; }
        else if (c == '"') { j = hl_strend(s, n, i); hl_paint(i, j, HC_STR); i = j; }
        else if (hl_lispword(c)) {
            j = i;
            while (j < n && hl_lispword(s[j])) ++j;
            if ((c >= '0' && c <= '9') || (c == '-' && j > i + 1 && s[i + 1] >= '0' && s[i + 1] <= '9') || c == '#')
                hl_paint(i, j, HC_NUM);
            else if (hl_inwords(s + i, j - i, lisp_kw)) hl_paint(i, j, HC_KW);
            i = j;
        } else ++i;
    }
}

// Forth: blank-separated words, case not mattering; \ and ( comment, S" ." C"
// ABORT" take a string, and : the name it defines
static void hl_forth(char *s, int n) {
    int i, j, k, c, def;
    char up[24];
    i = 0; def = 0;
    while (i < n) {
        if (s[i] == ' ' || s[i] == 9) { ++i; }
        else {
            j = i;
            while (j < n && s[j] != ' ' && s[j] != 9) ++j;
            k = 0;
            while (k < j - i && k < 23) { c = s[i + k]; up[k] = c >= 'a' && c <= 'z' ? c - 32 : c; ++k; }
            if (j - i == 1 && s[i] == 92) { hl_paint(i, n, HC_COM); j = n; }
            else if (j - i == 1 && s[i] == '(') {
                k = j; while (k < n && s[k] != ')') ++k;
                if (k < n) ++k;
                hl_paint(i, k, HC_COM); j = k;
            } else if (j - i <= 23 && j > i && s[j - 1] == '"' && (j - i == 2 || (j - i == 6 && up[0] == 'A'))) {
                hl_paint(i, j, HC_KW);
                k = j + 1; while (k < n && s[k] != '"') ++k;
                if (k < n) ++k;
                hl_paint(j, k, HC_STR); j = k;
            } else if (def) { hl_paint(i, j, HC_TYPE); def = 0; }
            else if ((s[i] >= '0' && s[i] <= '9') || (s[i] == '-' && j > i + 1 && s[i + 1] >= '0' && s[i + 1] <= '9'))
                hl_paint(i, j, HC_NUM);
            else if (j - i <= 23 && hl_inwords(up, j - i, forth_kw)) {
                hl_paint(i, j, HC_KW);
                if (j - i == 1 && s[i] == ':') def = 1;
            }
            i = j;
        }
    }
}

// shell, Makefiles and B4KE: # comments, quotes, $variables; a B4KE line's
// first word, in capitals, is its directive
static void hl_sh(char *s, int n, int b4k) {
    int i, j, c, first;
    i = 0; first = 1;
    while (i < n) {
        c = s[i];
        if (c == '#' && (i == 0 || s[i - 1] == ' ' || s[i - 1] == 9)) { hl_paint(i, n, HC_COM); i = n; }
        else if (c == '"' || c == 39) { j = hl_strend(s, n, i); hl_paint(i, j, HC_STR); i = j; first = 0; }
        else if (c == '$') {
            j = i + 1;
            if (j < n && (s[j] == '{' || s[j] == '(')) { c = s[j] == '{' ? '}' : ')'; while (j < n && s[j] != c) ++j; if (j < n) ++j; }
            else { while (j < n && hl_isword(s[j])) ++j; }
            hl_paint(i, j, HC_TYPE); i = j; first = 0;
        } else if (hl_isword(c)) {
            j = i;
            while (j < n && hl_isword(s[j])) ++j;
            if (b4k && first && c >= 'A' && c <= 'Z') hl_paint(i, j, HC_KW);
            else if (!b4k && hl_inwords(s + i, j - i, sh_kw)) hl_paint(i, j, HC_KW);
            else if (c >= '0' && c <= '9') hl_paint(i, j, HC_NUM);
            i = j; first = 0;
        } else { if (c != ' ' && c != 9) first = 0; ++i; }
    }
}

// Colour N characters of S as language LANG; ST is the state at its start
// (1: inside a C comment), and the state at its end comes back. hl_at
// then says what each character is (HC_*).
int hl_line(char *s, int n, int st, int lang) {
    hl_paint(0, n, HC_TEXT);
    if (lang == L_C || lang == L_JS) return hl_c(s, n, st, lang == L_JS);
    if (lang == L_LISP) hl_lisp(s, n);
    else if (lang == L_FORTH) hl_forth(s, n);
    else if (lang == L_SH || lang == L_B4K) hl_sh(s, n, lang == L_B4K);
    return 0;
}
int hl_at(int i) { return i < HL_MAX ? hlc[i] : HC_TEXT; }

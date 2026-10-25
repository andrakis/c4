// calc.c -- Calculator, a window kit class. docs/c4ix-desktop.md,
// docs/grin-plan.md 4.
//
// A programmer's calculator in three modes. Integer: 32-bit words shown
// in Hex, Dec, Oct or Bin, with the bitwise operators. Single and Double:
// IEEE floating point (docs/c4-float.md), each operation done in that
// precision -- c4lc's float and double, so in software unless the
// machine has an FPU -- with the value shown as the shortest decimal
// that reads back to it, and its bits under it. 0.1 + 0.2 is
// 0.30000000000000004 in Double and 0.3 in Single; Max * 2 is inf.
//
// The decimal conversions are libc's fp.c (src/c4ix/libc), linked into
// the shell: exact both ways. (fp.c sets errno, which the shell owns.)

#include "winkit.h"

int __c4ix_fmtfp(char *o, double x, int conv, int prec, int alt, int plus, int space);
double strtod(char *s, char **end);
float strtof(char *s, char **end);

enum { M_INT = 0, M_SINGLE = 1, M_DOUBLE = 2 };

struct calc {
    int acc, cur, op, radix, fresh, err;        // Integer mode's
    int lastop, lastb;                          // what = did, for = again
    int mode;
    double dacc, dcur;                          // Single and Double's (a Single value is exactly a double)
    double dlastb;
    int typed;                                  // a number being typed: TEXT is it
    char text[40];
    char disp[48];                              // what the display shows, and the bits line
    char bits[24];
};

static struct calc *C(int w) { return (struct calc *)wk_state(w); }
static char buf[64];

// the key grids: 6 columns, 5 rows
static char *keys[30] = {
    "D", "E", "F", "Mod", "And", "Not",
    "A", "B", "C", "/", "Or", "Xor",
    "7", "8", "9", "*", "Lsh", "Rsh",
    "4", "5", "6", "-", "+/-", "Back",
    "1", "2", "3", "+", "0", "="
};
static char *fkeys[30] = {
    "Sqrt", "x^2", "1/x", "Int", "Pi", "e",
    "Exp", "Max", "Eps", "/", "Min", "Inf",
    "7", "8", "9", "*", ".", "Abs",
    "4", "5", "6", "-", "+/-", "Back",
    "1", "2", "3", "+", "0", "="
};

static void refresh(struct calc *c);

static void calc_open_hook(int w, char *arg) {
    C(w)->radix = 10;
    C(w)->fresh = 1;
    refresh(C(w));
}

int calc_open() {
    calc_class.size = sizeof(struct calc);
    return wk_open(&calc_class, "Calculator", 360, 332, WK_FIXED, 0);
}

// ---- Integer -----------------------------------------------------------------------

static int apply(struct calc *c, int a, int b, int op) {
    if (op == '+') return a + b;
    if (op == '-') return a - b;
    if (op == '*') return a * b;
    if (op == '/' || op == '%') {
        if (!b) { c->err = 1; return 0; }
        return op == '/' ? a / b : a % b;
    }
    if (op == '&') return a & b;
    if (op == '|') return a | b;
    if (op == '^') return a ^ b;
    if (op == '<') return a << (b & 31);
    if (op == '>') {                         // logical: zeros come in from the top
        b = b & 31;
        if (!b) return a;
        return (a >> b) & (0x7fffffff >> (b - 1));
    }
    return b;
}

// the value in the current radix
static char *show(struct calc *c, char *out) {
    char t[40];
    int n, v, k, d;
    if (c->err) { ui_cpy(out, "Cannot divide by zero"); return out; }
    v = c->fresh && c->op ? c->acc : c->cur;
    if (c->radix == 10) return ui_num(out, v);
    n = 0;
    if (v == 0) { t[0] = '0'; n = 1; }
    while (v != 0 && n < 32) {
        d = v & (c->radix - 1);
        t[n] = d < 10 ? '0' + d : 'A' + d - 10;
        ++n;
        if (c->radix == 16) v = (v >> 4) & 0x0fffffff;
        else if (c->radix == 8) v = (v >> 3) & 0x1fffffff;
        else v = (v >> 1) & 0x7fffffff;
    }
    k = 0;
    while (n) { --n; out[k] = t[n]; ++k; }
    out[k] = 0;
    return out;
}

static int binop(char *key) {
    if (ui_eq(key, "+")) return '+';
    if (ui_eq(key, "-")) return '-';
    if (ui_eq(key, "*")) return '*';
    if (ui_eq(key, "/")) return '/';
    if (ui_eq(key, "Mod")) return '%';
    if (ui_eq(key, "And")) return '&';
    if (ui_eq(key, "Or")) return '|';
    if (ui_eq(key, "Xor")) return '^';
    if (ui_eq(key, "Lsh")) return '<';
    if (ui_eq(key, "Rsh")) return '>';
    if (ui_eq(key, "=")) return '=';
    return 0;
}

static void press_int(struct calc *c, char *key) {
    int d, op;
    d = 0 - 1;
    if (key[1] == 0 && key[0] >= '0' && key[0] <= '9') d = key[0] - '0';
    if (key[1] == 0 && key[0] >= 'A' && key[0] <= 'F') d = key[0] - 'A' + 10;
    if (d >= 0) {
        if (d >= c->radix) return;
        if (c->err) { c->err = 0; c->acc = 0; c->op = 0; }
        if (c->fresh) { c->cur = 0; c->fresh = 0; }
        c->cur = c->cur * c->radix + d;
        return;
    }
    if (ui_eq(key, "C")) { c->acc = 0; c->cur = 0; c->op = 0; c->lastop = 0; c->fresh = 1; c->err = 0; return; }
    if (ui_eq(key, "CE")) { c->cur = 0; c->fresh = 1; c->err = 0; return; }
    if (ui_eq(key, "Back")) { if (!c->fresh) c->cur = c->cur / c->radix; return; }
    if (ui_eq(key, "+/-")) { c->cur = 0 - c->cur; c->fresh = 0; return; }
    if (ui_eq(key, "Not")) { c->cur = ~c->cur; c->fresh = 0; return; }
    op = binop(key);
    if (!op) return;
    if (op == '=') {
        // = finishes what is pending ("5 + =" is 5 + 5), and = again does
        // the same to the answer: 2 + 3 = = is 8; a new number then = is
        // that number + 3
        if (c->op) {
            c->lastop = c->op;
            c->lastb = c->fresh ? c->acc : c->cur;
            c->acc = apply(c, c->acc, c->lastb, c->op);
        } else if (c->lastop) {
            if (!c->fresh) c->acc = c->cur;
            c->acc = apply(c, c->acc, c->lastb, c->lastop);
        } else c->acc = c->cur;
        c->cur = c->acc; c->op = 0; c->fresh = 1;
        return;
    }
    // a pending operation finishes first; two operators in a row replace
    if (c->op && !c->fresh) c->acc = apply(c, c->acc, c->cur, c->op);
    else if (!c->op) c->acc = c->cur;
    c->op = op;
    c->lastop = 0;
    c->fresh = 1;
}

// ---- Single and Double -------------------------------------------------------------

// a double rounded to single when the mode is Single
static double prec(struct calc *c, double v) { return c->mode == M_SINGLE ? (double)(float)v : v; }

static double c_sqrt(double x) {
    unsigned long long b;
    b = __c4d_sqrt(*(unsigned long long *)&x);
    return *(double *)&b;
}
static double c_bits(unsigned int hi, unsigned int lo) {
    unsigned long long b;
    b = ((unsigned long long)hi << 32) | lo;
    return *(double *)&b;
}

// each operation in the mode's own precision: float arithmetic in Single
static double fapply(struct calc *c, double a, double b, int op) {
    float x, y, r;
    if (c->mode == M_SINGLE) {
        x = (float)a; y = (float)b;
        if (op == '+') r = x + y;
        else if (op == '-') r = x - y;
        else if (op == '*') r = x * y;
        else if (op == '/') r = x / y;
        else r = y;
        return r;
    }
    if (op == '+') return a + b;
    if (op == '-') return a - b;
    if (op == '*') return a * b;
    if (op == '/') return a / b;
    return b;
}

static int c_has(char *s, int ch) { while (*s) { if (*s == ch) return 1; ++s; } return 0; }
static void c_parse(struct calc *c) {
    c->dcur = c->mode == M_SINGLE ? (double)strtof(c->text, 0) : strtod(c->text, 0);
}
static void c_append(struct calc *c, int ch) {
    int n;
    n = ui_len(c->text);
    if (n >= 32) return;
    c->text[n] = ch;
    c->text[n + 1] = 0;
}

// x towards zero, whole
static double c_trunc(double x) {
    unsigned long long b, m;
    int e;
    b = *(unsigned long long *)&x;
    e = (int)((b >> 52) & 2047) - 1023;
    if (e >= 52) return x;
    if (e < 0) return c_bits((unsigned int)(b >> 32) & 0x80000000, 0);
    m = 0x000FFFFFFFFFFFFFull >> e;
    b = b & ~m;
    return *(double *)&b;
}

// a unary key: the current value, replaced
static int funary(struct calc *c, char *key) {
    double v, r;
    int single;
    single = c->mode == M_SINGLE;
    v = c->dcur;
    if (ui_eq(key, "Sqrt")) r = c_sqrt(v);
    else if (ui_eq(key, "x^2")) r = fapply(c, v, v, '*');
    else if (ui_eq(key, "1/x")) r = fapply(c, 1.0, v, '/');
    else if (ui_eq(key, "Int")) r = c_trunc(v);
    else if (ui_eq(key, "Abs")) r = v < 0 ? 0.0 - v : v;
    else if (ui_eq(key, "Pi")) r = 3.14159265358979323846;
    else if (ui_eq(key, "e")) r = 2.71828182845904523536;
    else if (ui_eq(key, "Max")) r = single ? c_bits(0x47EFFFFF, 0xE0000000) : c_bits(0x7FEFFFFF, 0xFFFFFFFF);
    else if (ui_eq(key, "Min")) r = single ? c_bits(0x38100000, 0) : c_bits(0x00100000, 0);
    else if (ui_eq(key, "Eps")) r = single ? c_bits(0x3E800000, 0) : c_bits(0x3CB00000, 0);
    else if (ui_eq(key, "Inf")) r = c_bits(0x7FF00000, 0);
    else return 0;
    c->dcur = prec(c, r);
    c->typed = 0;
    c->fresh = 0;
    return 1;
}

static void press_fp(struct calc *c, char *key) {
    int op, n;
    if (key[1] == 0 && key[0] >= '0' && key[0] <= '9') {
        if (!c->typed) { c->text[0] = 0; c->typed = 1; c->fresh = 0; }
        c_append(c, key[0]);
        c_parse(c);
        return;
    }
    if (ui_eq(key, ".")) {
        if (!c->typed) { ui_cpy(c->text, "0"); c->typed = 1; c->fresh = 0; }
        if (!c_has(c->text, '.') && !c_has(c->text, 'e')) c_append(c, '.');
        c_parse(c);
        return;
    }
    if (ui_eq(key, "Exp")) {
        if (!c->typed) { ui_cpy(c->text, "1"); c->typed = 1; c->fresh = 0; }
        if (!c_has(c->text, 'e')) c_append(c, 'e');
        c_parse(c);
        return;
    }
    if (ui_eq(key, "C")) { c->dacc = 0; c->dcur = 0; c->op = 0; c->lastop = 0; c->fresh = 1; c->typed = 0; return; }
    if (ui_eq(key, "CE")) { c->dcur = 0; c->fresh = 1; c->typed = 0; return; }
    if (ui_eq(key, "Back")) {
        if (!c->typed) return;
        n = ui_len(c->text);
        if (n > 0) c->text[n - 1] = 0;
        c_parse(c);
        return;
    }
    if (ui_eq(key, "+/-")) {
        if (c->typed) {
            // the exponent's sign once one is being typed, else the number's
            n = 0;
            while (c->text[n] && c->text[n] != 'e') ++n;
            if (!c->text[n]) n = 0 - 1;
            if (c->text[n + 1] == '-') ui_cpy(c->text + n + 1, c->text + n + 2);
            else if (ui_len(c->text) < 32) {
                ui_cpy(buf, c->text + n + 1);
                c->text[n + 1] = '-';
                ui_cpy(c->text + n + 2, buf);
            }
            c_parse(c);
        } else { c->dcur = 0.0 - c->dcur; c->fresh = 0; }
        return;
    }
    if (funary(c, key)) return;
    op = binop(key);
    if (op != '+' && op != '-' && op != '*' && op != '/' && op != '=') return;
    c->typed = 0;
    if (op == '=') {
        // as Integer's: = again repeats the last operation on the answer
        if (c->op) {
            c->lastop = c->op;
            c->dlastb = c->fresh ? c->dacc : c->dcur;
            c->dacc = fapply(c, c->dacc, c->dlastb, c->op);
        } else if (c->lastop) {
            if (!c->fresh) c->dacc = c->dcur;
            c->dacc = fapply(c, c->dacc, c->dlastb, c->lastop);
        } else c->dacc = c->dcur;
        c->dcur = c->dacc; c->op = 0; c->fresh = 1;
        return;
    }
    if (c->op && !c->fresh) c->dacc = fapply(c, c->dacc, c->dcur, c->op);
    else if (!c->op) c->dacc = c->dcur;
    c->op = op;
    c->lastop = 0;
    c->fresh = 1;
}

static int c_toint(double v) {
    if (!(v == v)) return 0;
    if (v >= 2147483647.0) return 2147483647;
    if (v <= -2147483648.0) return 0 - 2147483647 - 1;
    return (int)v;
}

static void set_mode(struct calc *c, int mode) {
    if (mode == c->mode) return;
    if (c->mode == M_INT) { c->dacc = c->acc; c->dcur = c->cur; c->err = 0; }
    else if (mode == M_INT) { c->acc = c_toint(c->dacc); c->cur = c_toint(c->dcur); }
    c->mode = mode;
    c->lastop = 0;
    c->dacc = prec(c, c->dacc);
    c->dcur = prec(c, c->dcur);
    c->typed = 0;
}

// the shortest %g that reads back as the value (9 digits always do for a
// single, 17 for a double), and the bits
static char *hex8(char *o, unsigned int v) {
    int i, d;
    i = 0;
    while (i < 8) { d = (v >> (28 - 4 * i)) & 15; o[i] = d < 10 ? '0' + d : 'A' + d - 10; ++i; }
    o[8] = 0;
    return o + 8;
}
static void refresh(struct calc *c) {
    double v, back;
    unsigned long long b;
    float f;
    int p, maxp;
    char *o;
    if (c->mode == M_INT) { show(c, c->disp); c->bits[0] = 0; return; }
    v = c->fresh && c->op ? c->dacc : c->dcur;
    if (c->typed && !(c->fresh && c->op)) ui_cpy(c->disp, c->text);
    else {
        maxp = c->mode == M_SINGLE ? 9 : 17;
        p = 1;
        while (p < maxp) {
            __c4ix_fmtfp(c->disp, v, 'g', p, 0, 0, 0);
            back = c->mode == M_SINGLE ? (double)strtof(c->disp, 0) : strtod(c->disp, 0);
            if (back == v) break;
            ++p;
        }
        __c4ix_fmtfp(c->disp, v, 'g', p, 0, 0, 0);
    }
    if (c->mode == M_SINGLE) {
        f = (float)v;
        hex8(c->bits, *(unsigned int *)&f);
    } else {
        b = *(unsigned long long *)&v;
        o = hex8(c->bits, (unsigned int)(b >> 32));
        *o = ' ';
        hex8(o + 1, (unsigned int)b);
    }
}

static void press(struct calc *c, char *key) {
    if (c->mode == M_INT) press_int(c, key); else press_fp(c, key);
    refresh(c);
}

// ---- drawing and input -------------------------------------------------------------

enum { KW = 50, KH = 30, KX = 12, KY = 128 };
static char *radix_names[4] = { "Hex", "Dec", "Oct", "Bin" };
static int radix_values[4] = { 16, 10, 8, 2 };
static char *mode_names[3] = { "Integer", "Single", "Double" };

static void calc_draw(int w, int cw, int ch, int focused) {
    struct calc *c;
    int k, x, y;
    char **ks;
    c = C(w);
    d_rect(0, 0, cw, ch, C_FACE);
    // the display: right-aligned in a sunken white field
    d_rect(10, 10, cw - 20, 24, C_WHITE);
    d_bevel(10, 10, cw - 20, 24, B_SUNKEN);
    d_textn(cw - 16 - ui_len(c->disp) * 8, 15, C_DARK, 14, F_MONO, 8, c->disp, 0 - 1);
    // the radix, or the bits of a float
    ui_group(10, 40, 214, 44, "");
    if (c->mode == M_INT) {
        k = 0;
        while (k < 4) { ui_radio(20 + k * 50, 60, radix_names[k], c->radix == radix_values[k]); ++k; }
    } else {
        d_text(20, 50, C_SHADOW, F_SANS, c->mode == M_SINGLE ? "IEEE single, bits:" : "IEEE double, bits:");
        d_textn(20, 66, C_DARK, 12, F_MONO, 7, c->bits, 0 - 1);
    }
    ui_button(cw - 118, 50, 50, 28, "CE", 0, 0);
    ui_button(cw - 62, 50, 50, 28, "C", 0, 0);
    // the mode
    ui_group(10, 88, cw - 20, 32, "");
    k = 0;
    while (k < 3) { ui_radio(20 + k * 90, 100, mode_names[k], c->mode == k); ++k; }
    ks = c->mode == M_INT ? keys : fkeys;
    k = 0;
    while (k < 30) {
        x = KX + (k % 6) * (KW + 6);
        y = KY + (k / 6) * (KH + 6);
        ui_button(x, y, KW, KH, ks[k], 0, 0);
        ++k;
    }
    if (c->op) { buf[0] = c->op == '<' ? 'L' : c->op == '>' ? 'R' : c->op; buf[1] = 0; d_text(14, 16, C_SHADOW, F_SANS, buf); }
}

static void calc_mouse(int w, int ev, int x, int y, int b) {
    struct calc *c;
    int k, cw, ch;
    c = C(w);
    if (ev != E_DOWN) return;
    wk_size(w, &cw, &ch);
    k = 0;
    while (k < 4 && c->mode == M_INT) {
        if (ui_hit(x, y, 20 + k * 50, 58, 46, 16)) { c->radix = radix_values[k]; refresh(c); return; }
        ++k;
    }
    k = 0;
    while (k < 3) {
        if (ui_hit(x, y, 20 + k * 90, 98, 80, 16)) { set_mode(c, k); refresh(c); return; }
        ++k;
    }
    if (ui_hit(x, y, cw - 118, 50, 50, 28)) { press(c, "CE"); return; }
    if (ui_hit(x, y, cw - 62, 50, 50, 28)) { press(c, "C"); return; }
    k = 0;
    while (k < 30) {
        if (ui_hit(x, y, KX + (k % 6) * (KW + 6), KY + (k / 6) * (KH + 6), KW, KH)) {
            press(c, c->mode == M_INT ? keys[k] : fkeys[k]);
            return;
        }
        ++k;
    }
}

static void calc_key(int w, int down, int code, int ch, int mods) {
    struct calc *c;
    char k[2];
    if (!down) return;
    c = C(w);
    k[1] = 0;
    if (c->mode != M_INT) {
        if (ch == '.' || ch == ',') { press(c, "."); return; }
        if (ch == 'e' || ch == 'E') { press(c, "Exp"); return; }
        if (ch == 'p' || ch == 'P') { press(c, "Pi"); return; }
        if (ch == 'r' || ch == 'R') { press(c, "Sqrt"); return; }
    }
    if (ch >= 'a' && ch <= 'f') ch = ch - 32;
    if ((ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'F')) { k[0] = ch; press(c, k); return; }
    if (ch == '+' || ch == '-' || ch == '*' || ch == '/') { k[0] = ch; press(c, k); return; }
    if (ch == '=' || code == KEY_ENTER) { press(c, "="); return; }
    if (ch == '%') { press(c, "Mod"); return; }
    if (ch == '&') { press(c, "And"); return; }
    if (ch == '|') { press(c, "Or"); return; }
    if (ch == '^') { press(c, "Xor"); return; }
    if (ch == '~') { press(c, "Not"); return; }
    if (ch == '<') { press(c, "Lsh"); return; }
    if (ch == '>') { press(c, "Rsh"); return; }
    if (code == KEY_BACK) { press(c, "Back"); return; }
    if (code == KEY_ESC) { press(c, "C"); return; }
    if (code == KEY_DEL) { press(c, "CE"); return; }
}

struct wk_class calc_class = {
    .kind = ICON_CALC, .open = calc_open_hook, .draw = calc_draw, .mouse = calc_mouse, .key = calc_key
};

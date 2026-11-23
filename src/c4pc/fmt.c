/* fmt.c -- the PRTF formatter: libjs/printf.js in C.
 *
 * Every argument is one 32-bit word. %f %e %g read it as a binary32's
 * bits and print it as JavaScript's toFixed, toExponential and
 * toPrecision would: from the number's exact decimal expansion, rounded
 * half up, which is what those three do.
 *
 * On the 64-bit machine (C4PC_W64) an argument is a 64-bit word, and the
 * conversions take what native c4m's printf (the host's, given the words)
 * takes: %d %u %x %o %c the low 32 bits, with l, ll, j, z, t or q the
 * whole word; %p and %s the whole word; %f a binary32's bits as before.
 */
#include "c4pc.h"
#include "clib.h"

void ob_put(obuf *o, int c) {
  if (o->n == o->cap) { o->cap = o->cap ? o->cap * 2 : 256; o->b = xrealloc(o->b, o->cap); }
  o->b[o->n++] = (u8)c;
}
void ob_puts(obuf *o, const char *s) { while (*s) ob_put(o, *s++); }

/* ---- the exact decimal expansion of a finite double >= 0 ---- */
#define NL 40                         /* limbs: enough for a float's 2^128 and 2^-149 */
typedef struct { u32 l[NL]; } big;

static void big_mul10(big *b) {
  u32 carry = 0;
  for (int i = 0; i < NL; i++) { uint64_t t = (uint64_t)b->l[i] * 10 + carry; b->l[i] = (u32)t; carry = (u32)(t >> 32); }
}
static int big_zero(const big *b) { for (int i = 0; i < NL; i++) if (b->l[i]) return 0; return 1; }
static u32 big_div10(big *b) {
  uint64_t r = 0;
  for (int i = NL - 1; i >= 0; i--) { uint64_t t = (r << 32) | b->l[i]; b->l[i] = (u32)(t / 10); r = t % 10; }
  return (u32)r;
}
static void big_set_shl(big *b, uint64_t v, int sh) {       /* b = v << sh */
  memset(b, 0, sizeof *b);
  int w = sh >> 5, s = sh & 31;
  uint64_t lo = v << s, hi = s ? v >> (64 - s) : 0;
  if (w < NL) b->l[w] = (u32)lo;
  if (w + 1 < NL) b->l[w + 1] = (u32)(lo >> 32);
  if (w + 2 < NL) b->l[w + 2] = (u32)hi;
}
/* digits of v: ip the integer part (no leading zeros, "0" for none), fp
 * every digit of the fraction (exact: a binary fraction ends) */
static void exact_dec(double v, char *ip, int *ni, char *fp, int *nf) {
  union { double d; uint64_t u; } x; x.d = v;
  int ex = (int)((x.u >> 52) & 0x7ff);
  uint64_t mant = x.u & 0xfffffffffffffull;
  if (ex) mant |= 1ull << 52;
  int e = (ex ? ex : 1) - 1075;
  while (mant && !(mant & 1) && e < 0) { mant >>= 1; e++; }
  *ni = *nf = 0;
  big b;
  uint64_t ipart, frac = 0; int k = 0;
  if (e >= 0) { big_set_shl(&b, mant, e); }
  else {
    k = -e;
    ipart = k >= 64 ? 0 : mant >> k;
    frac = k >= 64 ? mant : mant & ((1ull << k) - 1);
    big_set_shl(&b, ipart, 0);
  }
  char tmp[400]; int nt = 0;
  if (big_zero(&b)) tmp[nt++] = '0';
  while (!big_zero(&b)) tmp[nt++] = (char)('0' + big_div10(&b));
  for (int i = 0; i < nt; i++) ip[i] = tmp[nt - 1 - i];
  *ni = nt;
  if (k) {
    /* r = frac / 2^k: each digit is (r * 10) >> k */
    big r; big_set_shl(&r, frac, 0);
    int wk = k >> 5, sk = k & 31;
    for (int n = 0; n < k && n < 1100; n++) {
      big_mul10(&r);
      /* the digit: bits from k up */
      u32 d = (u32)(((uint64_t)(wk + 1 < NL ? r.l[wk + 1] : 0) << 32 | r.l[wk]) >> sk) & 15;
      fp[(*nf)++] = (char)('0' + d);
      /* r &= 2^k - 1 */
      r.l[wk] &= sk ? (1u << sk) - 1 : 0;
      for (int i = wk + 1; i < NL; i++) r.l[i] = 0;
      if (big_zero(&r)) break;
    }
  }
}

/* round the digit string d[0..n) at position p (keep p digits), half up
 * on the next digit: a carry may make it one longer (returns 1) */
static int round_at(char *d, int n, int p) {
  if (p >= n || d[p] < '5') return 0;
  int i = p - 1;
  while (i >= 0 && d[i] == '9') d[i--] = '0';
  if (i >= 0) { d[i]++; return 0; }
  memmove(d + 1, d, (size_t)p);
  d[0] = '1';
  return 1;
}

static void put_exp(char *s, int *n, int e, int two) {
  s[(*n)++] = 'e';
  s[(*n)++] = e < 0 ? '-' : '+';
  if (e < 0) e = -e;
  char t[8]; int k = 0;
  do { t[k++] = (char)('0' + e % 10); e /= 10; } while (e);
  if (two && k < 2) t[k++] = '0';
  while (k) s[(*n)++] = t[--k];
}

/* the significant digits of v rounded to p, and the exponent of the
 * first: v ~ 0.d1d2... * 10^(e+1) */
static int sig_digits(double v, int p, char *out, int *e) {
  char ip[400], fp[1200]; int ni, nf;
  exact_dec(v, ip, &ni, fp, &nf);
  char all[1600]; int n = 0, point;
  int lead = 0;
  if (!(ni == 1 && ip[0] == '0')) { memcpy(all, ip, (size_t)ni); n = ni; point = ni; }
  else {
    point = 0;
    while (lead < nf && fp[lead] == '0') lead++;
    point = -lead;
  }
  memcpy(all + n, fp + (n ? 0 : lead), (size_t)(nf - (n ? 0 : lead))); n += nf - (n ? 0 : lead);
  if (n == 0) { for (int i = 0; i < p; i++) out[i] = '0'; *e = 0; return p; }
  while (n < p + 1) all[n++] = '0';
  int carry = round_at(all, n, p);
  memcpy(out, all, (size_t)p);
  *e = point - 1 + carry;
  return p;
}

static int js_fixed(double v, int p, char *s) {
  char ip[400], fp[1200]; int ni, nf, n = 0;
  exact_dec(v, ip, &ni, fp, &nf);
  char d[1600]; int nd = 0;
  memcpy(d, ip, (size_t)ni); nd = ni;
  for (int i = 0; i < p; i++) d[nd++] = i < nf ? fp[i] : '0';
  d[nd] = p < nf ? fp[p] : '0';
  /* a carry out of the top only comes from all nines ("99.96" -> "100.0"):
   * an integer part of 0 takes the carry itself ("0.96" -> "1.0") */
  int carry = round_at(d, nd + 1, nd);
  int intd = ni + carry;
  for (int i = 0; i < intd; i++) s[n++] = d[i];
  if (p) { s[n++] = '.'; for (int i = 0; i < p; i++) s[n++] = d[intd + i]; }
  return n;
}

static int js_exponential(double v, int p, char *s) {
  char d[1200]; int e, n = 0;
  sig_digits(v, p + 1, d, &e);
  if (v == 0) e = 0;
  s[n++] = d[0];
  if (p) { s[n++] = '.'; for (int i = 1; i <= p; i++) s[n++] = d[i]; }
  put_exp(s, &n, e, 1);
  return n;
}

/* String(Number(v.toPrecision(p))): p significant digits, then as
 * JavaScript prints that number, trailing zeros gone */
static int js_general(double v, int p, char *s) {
  char d[1200]; int e, n = 0;
  if (v == 0) { s[0] = '0'; return 1; }
  sig_digits(v, p, d, &e);
  int nd = p;
  while (nd > 1 && d[nd - 1] == '0') nd--;
  if (e >= 21 || e < -6) {
    s[n++] = d[0];
    if (nd > 1) { s[n++] = '.'; for (int i = 1; i < nd; i++) s[n++] = d[i]; }
    put_exp(s, &n, e, 0);
  } else if (e < 0) {
    s[n++] = '0'; s[n++] = '.';
    for (int i = 0; i < -e - 1; i++) s[n++] = '0';
    for (int i = 0; i < nd; i++) s[n++] = d[i];
  } else {
    for (int i = 0; i <= e; i++) s[n++] = i < nd ? d[i] : '0';
    if (nd > e + 1) { s[n++] = '.'; for (int i = e + 1; i < nd; i++) s[n++] = d[i]; }
  }
  return n;
}

/* in the word's arithmetic: the 32-bit PC has no 64-bit division of its own */
static int digits(char *s, uword v, int base, int upper, int prec) {
  char t[40]; int k = 0, n = 0;
  if (prec == 0 && v == 0) return 0;
  do { int d = (int)(v % (uword)base); t[k++] = (char)(d < 10 ? '0' + d : (upper ? 'A' : 'a') + d - 10); v /= (uword)base; } while (v);
  for (int z = prec - k; z > 0 && n < 512; z--) s[n++] = '0';
  while (k) s[n++] = t[--k];
  return n;
}

void fmt_format(obuf *out, const u8 *fmt, u32 n, const word *args, int nargs,
                u32 (*str)(void *ctx, uword addr, const u8 **p), void *ctx) {
  int ai = 0;
#define NEXT() (ai < nargs ? args[ai++] : 0)
#define AT(j) ((j) < n ? fmt[j] : 0)
  u32 i = 0;
  static char body[2048];
  while (i < n) {
    int c = fmt[i++];
    if (c != '%') { ob_put(out, c); continue; }
    if (i >= n) { ob_put(out, '%'); break; }
    int left = 0, plus = 0, space = 0, zero = 0, alt = 0;
    for (;;) {
      int f = AT(i);
      if (f == '-') left = 1; else if (f == '+') plus = 1; else if (f == ' ') space = 1;
      else if (f == '0') zero = 1; else if (f == '#') alt = 1; else break;
      i++;
    }
    i32 width = 0;
    if (AT(i) == '*') { width = (i32)NEXT(); i++; if (width < 0) { left = 1; width = -width; } }
    else while (AT(i) >= '0' && AT(i) <= '9') width = width * 10 + (AT(i++) - '0');
    i32 prec = -1;
    if (AT(i) == '.') {
      i++; prec = 0;
      if (AT(i) == '*') { prec = (i32)NEXT(); i++; if (prec < 0) prec = -1; }
      else while (AT(i) >= '0' && AT(i) <= '9') prec = prec * 10 + (AT(i++) - '0');
    }
    int len = 0, lng = 0;
    for (;;) {
      int l = AT(i);
      if (l == 'h') { len = len == 1 ? 2 : 1; i++; }
      else if (l == 'l' || l == 'L' || l == 'q' || l == 'j' || l == 'z' || l == 't') { lng = 1; i++; }
      else break;
    }
    /* a word as the conversion takes it: all of it with l (or on the 32-bit
     * machine, where that is the same), else its low 32 bits */
#define SIGNED(w) ((lng || WB == 4) ? (i64)(w) : (i64)(i32)(w))
#define UNSIGNED(w) ((lng || WB == 4) ? (uint64_t)(uword)(w) : (uint64_t)(u32)(w))
    int conv = i < n ? fmt[i] : -1; i++;
    int nb = 0, numeric = 0;
    const u8 *bp = (const u8 *)body;
    char prefix[4]; int np = 0;
    switch (conv) {
      case 'd': case 'i': {
        i64 v = SIGNED(NEXT());
        if (len == 1) v = (int16_t)v; else if (len == 2) v = (int8_t)v;
        int neg = v < 0;
        nb = digits(body, (uword)(neg ? (uint64_t)0 - (uint64_t)v : (uint64_t)v), 10, 0, prec);
        if (neg) prefix[np++] = '-'; else if (plus) prefix[np++] = '+'; else if (space) prefix[np++] = ' ';
        numeric = 1;
        break;
      }
      case 'u': case 'x': case 'X': case 'o': {
        uword v = (uword)UNSIGNED(NEXT());
        if (len == 1) v &= 0xffff; else if (len == 2) v &= 0xff;
        int base = conv == 'o' ? 8 : conv == 'u' ? 10 : 16;
        nb = digits(body + 1, v, base, conv == 'X', prec);
        bp = (const u8 *)body + 1;
        if (alt && v && conv == 'x') { prefix[np++] = '0'; prefix[np++] = 'x'; }
        if (alt && v && conv == 'X') { prefix[np++] = '0'; prefix[np++] = 'X'; }
        if (alt && conv == 'o' && (nb == 0 || body[1] != '0')) { body[0] = '0'; bp = (const u8 *)body; nb++; }
        numeric = 1;
        break;
      }
      case 'c': body[0] = (char)(NEXT() & 0xff); nb = 1; break;
      case 's': {
        uword p = (uword)NEXT();
        if (!p) { if (prec >= 0 && prec < 6) nb = 0; else { memcpy(body, "(null)", 6); nb = 6; } }
        else {
          const u8 *sp; u32 sl = str(ctx, p, &sp);
          if (prec >= 0 && (u32)prec < sl) sl = (u32)prec;
          bp = sp; nb = (int)sl;
        }
        break;
      }
      case 'p': {
        uword p = (uword)NEXT();
        if (p) { prefix[np++] = '0'; prefix[np++] = 'x'; nb = digits(body, p, 16, 0, prec); numeric = 1; }
        else { memcpy(body, "(nil)", 5); nb = 5; }
        break;
      }
      case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': {
        float fv = f32_of((i32)NEXT());
        double v = fv;
        int p = prec < 0 ? 6 : prec;
        if (p > 100) p = 100;
        char *s = body; int sl;
        int neg = 0;
        if (v != v) { memcpy(s, "nan", 3); sl = 3; }
        else if (v == 1.0 / 0.0 || v == -1.0 / 0.0) { if (v < 0) { neg = 1; } memcpy(s, "inf", 3); sl = 3; }
        else {
          if (v < 0 || (v == 0 && (bits_of_f32(fv) < 0))) { neg = v < 0; v = -v; }
          if (conv == 'f' || conv == 'F') sl = js_fixed(v, p, s);
          else if (conv == 'e' || conv == 'E') sl = js_exponential(v, p, s);
          else sl = js_general(v, p ? p : 1, s);
          /* JS keeps no "-" for a result that rounds to zero? It does: (-0.001).toFixed(2) is "-0.00" */
        }
        if (conv == 'F' || conv == 'E' || conv == 'G') for (int k = 0; k < sl; k++) if (s[k] >= 'a' && s[k] <= 'z') s[k] -= 32;
        if (neg) prefix[np++] = '-'; else if (plus) prefix[np++] = '+'; else if (space) prefix[np++] = ' ';
        nb = sl; numeric = 1; prec = -1;
        break;
      }
      case 'n': (void)NEXT(); continue;
      case '%': ob_put(out, '%'); continue;
      default:
        ob_put(out, '%');
        if (conv >= 0) ob_put(out, conv);
        continue;
    }
    i32 pad = width - nb - np;
    int zero_pad = zero && !left && numeric && prec < 0;
    if (pad > 0 && !left && !zero_pad) for (i32 k = 0; k < pad; k++) ob_put(out, ' ');
    for (int k = 0; k < np; k++) ob_put(out, prefix[k]);
    if (pad > 0 && zero_pad) for (i32 k = 0; k < pad; k++) ob_put(out, '0');
    for (int k = 0; k < nb; k++) ob_put(out, bp[k]);
    if (pad > 0 && left) for (i32 k = 0; k < pad; k++) ob_put(out, ' ');
  }
#undef NEXT
#undef AT
#undef SIGNED
#undef UNSIGNED
}

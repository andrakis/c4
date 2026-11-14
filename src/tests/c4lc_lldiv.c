// c4lc_lldiv.c -- 64-bit division and remainder, signed and unsigned, every
// divisor size (__c4ll_udivmod and its fast path for a divisor below 2^30).
// gcc wrote the golden (c4lc-lldiv.txt).
unsigned seed;
unsigned rnd() { seed = seed * 1103515245 + 12345; return seed; }
int main() {
  int i, sh; long long a, b, q, r, sq, sr; unsigned long long ua, ub, uq, ur;
  unsigned long long h1, h2;
  h1 = 0; h2 = 0; seed = 7;
  for (i = 0; i < 3000; i++) {
    ua = ((unsigned long long)rnd() << 32) | rnd();
    sh = rnd() % 64; ua = ua >> sh;
    ub = ((unsigned long long)rnd() << 32) | rnd();
    sh = rnd() % 64; ub = ub >> sh;
    if (i % 5 == 0) ub = ub & 0x3fffffff;
    if (i % 7 == 0) ub = ub & 0xffff;
    if (i % 11 == 0) ub = 1 + (rnd() & 0x3fffffff);
    if (i % 13 == 0) ub = 0x3fffffff;
    if (i % 17 == 0) ub = 0x40000000;
    if (ub == 0) ub = 3;
    uq = ua / ub; ur = ua % ub;
    h1 = h1 * 1000003 + uq; h1 = h1 * 1000003 + ur;
    a = (long long)ua; b = (long long)ub;
    if (i & 1) a = -a; if (i & 2) b = -b;
    sq = a / b; sr = a % b;
    h2 = h2 * 1000003 + (unsigned long long)sq; h2 = h2 * 1000003 + (unsigned long long)sr;
  }
  printf("%08x%08x %08x%08x\n", (unsigned)(h1 >> 32), (unsigned)h1, (unsigned)(h2 >> 32), (unsigned)h2);
  return 0;
}

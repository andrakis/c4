// c4lc L11: integer constant expressions in enum bodies, and the
// widened p:const those share with array sizes, case labels and
// initializers. gcc is the oracle; c4cc's enum parser takes literal
// numbers only, so it cannot compile this at all.

// a constant named earlier in the SAME body is visible to the next
enum { A = 1, B = A + 1, C = B * 3, D = -C, E = A - B };
enum { SHIFT = 12, ONE = 1 << SHIFT, MASK = ONE - 1 };
enum { F1 = 1, F2 = 2, F4 = 4, ALL = F1 | F2 | F4, X = ALL ^ F2, Y = ALL & F4 };
enum { M = 17 % 5, N = ~0, P = !0, Q = !5, R = - -7, S = (A + B) * (C - 1) };
enum { BIG = 1 << 20, HALF = BIG >> 1, DIV = BIG / 1024 };

int tbl[1 << 3];          // array size takes the same expressions
int seeded[MASK & 7];

int classify (int v) {
  // case labels take them too
  if (v == ALL) return 100;
  if (v == X) return 200;
  return 0;
}

int main () {
  printf("A=%d B=%d C=%d D=%d E=%d\n", A, B, C, D, E);
  printf("SHIFT=%d ONE=%d MASK=%d\n", SHIFT, ONE, MASK);
  printf("ALL=%d X=%d Y=%d\n", ALL, X, Y);
  printf("M=%d N=%d P=%d Q=%d\n", M, N, P, Q);
  printf("R=%d S=%d BIG=%d HALF=%d DIV=%d\n", R, S, BIG, HALF, DIV);
  tbl[7] = 99;
  printf("tbl7=%d classify=%d %d\n", tbl[7], classify(ALL), classify(X));
  return 0;
}

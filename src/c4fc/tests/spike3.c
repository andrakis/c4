// c4fc F4: control flow. if/else, while, for, do-while, break, continue,
// the conditional, short-circuit && and ||, and every unary operator.
int f1(int x) { int r; r = 0; if (x) r = 1; return r; }
int f2(int x) { int r; if (x) r = 1; else r = 2; return r; }
int f3(int n) { int i; i = 0; while (i < n) i = i + 1; return i; }
int f4(int n) { int i, s; s = 0; for (i = 0; i < n; i = i + 1) s = s + i; return s; }
int f5(int n) { int i; i = 0; do { i = i + 1; } while (i < n); return i; }
int f6(int n) { int i; i = 0; while (1) { i = i + 1; if (i > n) break; if (i == 2) continue; } return i; }
int f7(int x) { return x ? 10 : 20; }
int f8(int n) { int i, s; s = 0; for (i = 0; i < n; i = i + 1) { if (i == 1) continue; s = s + i; } return s; }
int f9(int x) { return !x + ~x + -x; }
int f10(int *p) { return *p; }
int f11(int n) { int i; i = n; i++; ++i; i--; --i; return i; }
int f12(int a, int b) { return a && b || !a; }
int f13(int *p, int n) { *p = n; return *p; }
int f14(int x) { return -1 + sizeof(int) + sizeof(char); }
int f15(int n) { int i, j, s; s = 0; for (i = 0; i < n; i++) for (j = 0; j < n; j++) s = s + i * j; return s; }
int main()
{
  int v;
  v = 7;
  return f1(1) + f2(1) + f3(3) + f4(3) + f5(3) + f6(3) + f7(1) + f8(3)
       + f9(1) + f10(&v) + f11(1) + f12(1, 0) + f13(&v, 2) + f14(0) + f15(3);
}

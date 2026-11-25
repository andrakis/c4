// c4fc spike: arithmetic, comparison and bitwise operators, one row each
// in the infix table. Straight-line only -- control flow is F4.
int a, b, c, d;

int calc(int x, int y)
{
  int t;
  t = x * y + 3;
  t = t - x / 2;
  t = t % 7;
  t = t << 2;
  t = t >> 1;
  return t;
}

int cmp(int x, int y)
{
  int r;
  r = x < y;
  r = r + (x > y);
  r = r + (x == y);
  r = r + (x != y);
  r = r + (x <= y);
  r = r + (x >= y);
  return r;
}

int bits(int x) { return x & 3 | x ^ 5; }

int main()
{
  a = calc(6, 7);
  b = cmp(1, 2);
  c = bits(9);
  d = a + b * c;
  printf("%d %d %d %d\n", a, b, c, d);
  return 0;
}

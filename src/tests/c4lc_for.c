// c4lc: for-loops work (c4cc's never did -- its For branch dies on any
// use). Oracle: gcc compiles this same file; outputs must match.

int main()
{
  int i, j, sum;

  sum = 0;
  for (i = 0; i < 10; i++) sum = sum + i;
  printf("sum %d\n", sum);

  // continue lands on the step expression
  sum = 0;
  for (i = 0; i < 10; i++) {
    if (i % 2) continue;
    sum = sum + i;
  }
  printf("evens %d\n", sum);

  // nested, with break in the inner loop
  sum = 0;
  for (i = 1; i <= 4; i++) {
    for (j = 1; j <= 4; j++) {
      if (j > i) break;
      sum = sum + i * j;
    }
  }
  printf("nested %d\n", sum);

  // empty sections: init and step hoisted out, bare condition
  i = 3;
  for (; i > 0;) {
    i = i - 1;
  }
  printf("drained %d\n", i);

  // empty condition runs until break
  sum = 0;
  for (i = 0; ; i++) {
    if (i == 5) break;
    sum = sum + 1;
  }
  printf("counted %d\n", sum);

  return 0;
}

// A basic test
#include <stdio.h>

int add (int a, int b) { return a + b; }

int main (int argc, char **argv) {
  int a, b, c;
  a = 2;
  b = 3;
  printf("A basic test of %d + %d:\n", a, b);
  c = add(a, b);
  printf("  %d\n", c);
  return 0;
}

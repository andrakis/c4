// c4lc L0 lexer sample: every token kind and lexer quirk in one file.
// The golden dump lives in src/c4sp/tests/expected/c4lc-tokens.txt.
#include <stdio.h>

enum { RED, GREEN = 5, BLUE };

int global;
char *msg = "hello\nworld";
char *empty = "";
char *quirks = "tab\there\rcr\0nul\\slash\"quote";

/* block comment
   spanning lines: the token after it must report the right line */
int after_block;

int add(int a, int b) { return a + b; }

int variadic(int n, ...) { return n; }

int proto(char *s, int n);
static int hidden;
extern int elsewhere;
int *fnptr = &add;
int table[3] = { RED, -2, 0x10 };
char word[] = "abc";
char pad[8];

void __attribute__((constructor)) setup () { global = 1; }

int loops(int n)
{
  int i, sum;
  int acc[4] = { 1, -1 };
  char tag[6] = "ok";
  sum = 0;
  for (i = 0; i < n; ++i) {
    if (i == 2) continue;
    sum = sum + acc[i % 4] + tag[0];
  }
  while (sum > 100) { sum--; break; }
  return sum;
}

int main()
{
  int i, x;
  char c;
  i = 42;
  i = 0x2A;
  i = 052;
  i = 0;
  c = 'A';
  c = '\n';
  c = '';
  c = 'ab';
  x = i++ + --i;
  x = i << 2 >> 1;
  x = i < 1 | i > 2 ^ i <= 3 & i >= 4;
  x = i == 1 || i != 2 && !i;
  x = ~i % 3 * 2 / 1 - -1;
  x = i ? add(1, 2) : global;
  if (x) { x = x; } else x = 0;
  while (x > 0) x--;
  switch (x) {
  case 0: break;
  case GREEN: break;
  default: break;
  }
  return sizeof(int) + sizeof(char *);
}

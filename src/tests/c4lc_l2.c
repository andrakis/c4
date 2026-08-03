// c4lc L2: everything the minimal code generator covers, in one file
// that c4cc also compiles -- the test requires identical output from
// both compilers' binaries under c4m.

enum { RED, GREEN = 5, BLUE };

int counter;
int scale = 3;
char letter = 'A';
char *greeting = "greetings";
int fact(int n);

int fact(int n)
{
  if (n < 2) return 1;
  return n * fact(n - 1);
}

// c4cc requires the target already defined, so this sits below fact
int *fp = &fact;

int twice(int x) { return x + x; }

int gcd(int a, int b)
{
  int t;
  while (b) { t = b; b = a % b; a = t; }
  return a;
}

int side(int v) { counter = counter + v; return v; }

void bump() { ++counter; }

int main()
{
  int i, x;
  int start = 40;
  char *msg = "local string";
  int *p;

  printf("fact(10) = %d\n", fact(10));
  printf("gcd(%d, %d) = %d\n", 1071, 462, gcd(1071, 462));
  printf("twice: %d %d\n", twice(21), twice(GREEN));

  // globals, initialized and mutated
  printf("scale %d letter %c %s\n", scale, letter, greeting);
  counter = 0;
  bump(); bump(); bump();
  printf("counter %d\n", counter);

  // short-circuit evaluation must skip side effects
  counter = 0;
  x = 0 && side(1);
  x = 1 || side(2);
  x = 1 && side(4);
  x = 0 || side(8);
  printf("short-circuit %d after %d %d\n", counter, x, 0 ? side(16) : 3);

  // pre/post increment, compound expression parens
  i = 10;
  x = i++ + ++i;
  x = (x = x * 2, x + i);
  printf("incdec %d %d\n", i, x);

  // pointers to locals and globals
  p = &start;
  *p = *p + 2;
  p = &counter;
  *p = 99;
  printf("ptrs %d %d %s\n", start, counter, msg);

  // char handling and casts
  letter = letter + 1;
  x = (int)letter * 2;
  printf("char %c %d %d\n", letter, x, (int)'z');

  // function pointers: global holding &fact, local copy
  x = fp(6);
  p = fp;
  i = p(5);
  printf("fnptr %d %d\n", x, i);

  // ternaries, logic, bit ops, shifts, negation
  x = start > 41 ? RED : BLUE;
  i = ~x & 15 | 32 ^ 7;
  x = -i + (1 << 6) - (256 >> 2);
  printf("ops %d %d %d %d\n", x, i, !x, -x);

  // while with break/continue
  i = 0; x = 0;
  while (i < 10) {
    i = i + 1;
    if (i == 3) continue;
    if (i == 8) break;
    x = x + i;
  }
  printf("loop %d %d\n", i, x);

  // sizeof
  printf("sizes %d %d %d\n", sizeof(int), sizeof(char), sizeof(char *));

  puts(greeting);
  putchar('o'); putchar('k'); putchar(10);
  return 0;
}

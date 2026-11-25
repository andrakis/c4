// c4fc spike: globals laid out after every string, calls between
// functions, casts, and a char local.
char *greeting;
int count;

int twice(int n) { return n + n; }
int thrice(int n) { return twice(n) + n; }

int show(char *s, int n)
{
  printf("%s %d\n", s, n);
  return n;
}

int main(int argc, char **argv)
{
  int n;
  char c;
  greeting = "spike";
  count = 0;
  c = 65;
  n = (int)argv;
  n = thrice(7);
  count = show(greeting, n);
  count = count + c;
  printf("done %d\n", count);
  return 0;
}

#include <stdio.h>

int add (int a, int b) { return a + b; }

int main(int argc, char **argv)
{
  int i;
  printf("Argument count: %ld\n", argc);
  printf("2 + 2: %ld\n", add(2, 2));
  i = 0;
  while(i < argc) {
    printf("Argument %ld: ", i);
    printf("'%s'\n", argv[i++]);
  }
  return 0;
}


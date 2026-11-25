struct s { int a; int b; char c[8]; };
int main() { struct s x; struct s y; x.a = 1; x.b = 2; memcpy(x.c, "hi", 3); y = x;
  printf("copy %d %d %s\n", y.a, y.b, y.c); return 0; }

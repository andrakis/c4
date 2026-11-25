int main() { int i; int s; s = 0;
  for (i = 0; i < 4; i++) switch (i) { case 0: s += 1; case 1: s += 10; break; default: s += 100; }
  printf("sw %d\n", s); return 0; }

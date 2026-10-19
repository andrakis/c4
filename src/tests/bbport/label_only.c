int main() { int i; i = 0;
top:
  i++;
  if (i < 3) goto top;
  printf("lo %d\n", i); return 0; }

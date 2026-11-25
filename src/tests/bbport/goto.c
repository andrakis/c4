int main() { int i; i = 0;
again: i = i + 1; if (i < 5) goto again;
  if (i == 5) goto done; printf("bad\n");
done: printf("goto %d\n", i); return 0; }

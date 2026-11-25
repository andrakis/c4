// c4lc_u8.c -- LCU (docs/c4-16bit.md): unsigned char loads, beside signed
// and plain chars, in arithmetic, through pointers, ++ and the values of
// assignments. gcc wrote the golden (c4lc-u8.txt); any word size.
unsigned char ub[40]; signed char sb[40]; char cb[40];
int usum;
int umix(int v) { usum = usum * 33 + v; return v; }
int main() {
  int i, v; unsigned char *p, c; unsigned char *q;
  for (i = 0; i < 40; i++) { ub[i] = i * 29 + 200; sb[i] = i * 29 + 200; cb[i] = i * 29 + 200; }
  for (i = 0; i < 40; i++) { umix(ub[i]); umix(sb[i]); umix(cb[i]); umix(ub[i] + sb[i] - cb[i]); }
  p = ub; for (i = 0; i < 39; i++) { c = *p++; umix(c); umix(*p * 3); umix(ub[i] > 127); }
  q = ub + 5; v = (*q)++; umix(v); umix(*q); v = ++(*q); umix(v); *q = 255; v = (*q)++; umix(v); umix(*q);
  c = 250; c += 10; umix(c); umix((unsigned char)(c - 20));
  printf("usum %d\n", usum);
  return 0;
}

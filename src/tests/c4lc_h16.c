// c4lc_h16.c -- LH LHU SH LSR (docs/c4-16bit.md): shorts at every alignment,
// the values of their assignments and post-increments, and an unsigned >>
// by a run-time count. gcc wrote the golden (c4lc-h16.txt); any word size.
struct s { char c; short a; unsigned short b; char d; short e[3]; };
char buf[64];
int sum;
int mix(int v) { sum = sum * 31 + v; return v; }
int main() {
  short *sp; unsigned short *up; int i, k, v; unsigned u, w; struct s st; struct s *p;
  for (i = 0; i < 64; i++) buf[i] = i * 37 + 11;
  for (i = 0; i < 8; i++) {
    sp = (short *)(buf + i); up = (unsigned short *)(buf + 9 + i);
    mix(*sp); mix(*up);
    v = (*sp = -12345 + i * 4097); mix(v); mix(*sp);
    v = (*up = 54321 + i * 99); mix(v); mix(*up);
    *sp = 70000 + i; mix(*sp); *up = -1 - i; mix(*up);
    (*sp)++; mix(*sp); ++(*up); mix(*up);
    *sp = 32767; v = (*sp)++; mix(v); mix(*sp);
    *up = 65535; v = (*up)++; mix(v); mix(*up);
  }
  p = &st; p->c = 1; p->a = -2; p->b = 40000; p->d = 3; p->e[0] = -300; p->e[1] = 300; p->e[2] = -1;
  mix(p->a); mix(p->b); mix(p->e[0] + p->e[1] + p->e[2]); p->b += p->a; mix(p->b); p->a <<= 7; mix(p->a);
  for (i = 0; i < 64; i++) mix(buf[i]);
  u = 0x80000001u; w = 0xdeadbeefu;
  for (k = 0; k < 32; k++) { mix((int)(u >> k)); mix((int)(w >> k)); mix((int)((unsigned)-k >> (k & 7))); }
  printf("sum %d\n", sum);
  for (k = 0; k < 8; k++) printf("%d %d %u %u\n", ((short *)buf)[k], ((unsigned short *)buf)[k], w >> (k * 4), u >> (k * 3 + 1));
  return 0;
}

// c4lc_opassign.c -- a compound assignment evaluates its left side once, whatever
// it does and whatever its type. test-c4lc holds c4lc's output to gcc's, and
// test-c4fc holds c4fc's, with C4FC defined: c4fc has no floating point, and
// its int is the 64-bit word, so (int) of a long long keeps all of it.
#include <stdio.h>

int n;
int f(void) { n = n + 1; return n; }

int a[8];
unsigned char s[8];
short h[8];
#ifndef C4FC
long long ll[4];
double d[4];
float g[4];
#endif
int *ip[4];
struct pt { int x, y; } pts[4];
struct pt *pp;

int main(void) {
    int i, r;
    int *p;
    unsigned char *q;

    a[f()] |= 1; printf("1: n=%d %d %d\n", n, a[1], a[2]);
    n = 0; a[f()] += 2; printf("2: n=%d %d %d\n", n, a[1], a[2]);
    n = 0; r = (a[f()] *= 5); printf("3: n=%d r=%d %d %d\n", n, r, a[1], a[2]);
    i = 0; a[i++] += 7; printf("4: i=%d %d %d\n", i, a[0], a[1]);
    i = 3; a[--i] -= 4; printf("5: i=%d %d %d\n", i, a[2], a[3]);

    // through a pointer that moves
    p = a + 4; *p++ += 9; printf("6: %d %d %d\n", (int)(p - a), a[4], a[5]);
    p = a + 6; *++p |= 6; printf("7: %d %d %d\n", (int)(p - a), a[6], a[7]);

    // the sizes: each wraps as its type does
    q = s; *q++ += 200; q = s; *q++ += 100; printf("8: %d %d %d\n", (int)(q - s), s[0], s[1]);
    n = 0; h[f()] -= 3; printf("9: n=%d %d %d\n", n, h[1], h[2]);
#ifndef C4FC
    n = 0; ll[f()] += 5000000000LL; printf("10: n=%d %d %d %d\n", n, (int)(ll[1] >> 32), (int)ll[1], (int)ll[2]);
    n = 0; d[f()] += 1.5; d[1] *= 2.0; printf("11: n=%d %g %g\n", n, d[1], d[2]);
    n = 0; g[f()] -= 2.5f; printf("12: n=%d %g %g\n", n, (double)g[1], (double)g[2]);
#endif

    // pointers scale, and members
    n = 0; ip[f()] += 3; printf("13: n=%d %d %d\n", n, (int)(ip[1] - (int *)0), (int)(ip[2] - (int *)0));
    n = 0; pts[f()].y += 8; printf("14: n=%d %d %d\n", n, pts[1].y, pts[2].y);
    pp = pts; (pp++)->x ^= 3; printf("15: %d %d %d\n", (int)(pp - pts), pts[0].x, pts[1].x);

    // inside a condition, and the shifts
    n = 0; if (a[f()] &= 3) printf("16: true n=%d %d\n", n, a[1]); else printf("16: false n=%d %d\n", n, a[1]);
    n = 0; a[f()] <<= 2; printf("17: n=%d %d\n", n, a[1]);
    n = 0; a[f()] >>= 1; printf("18: n=%d %d\n", n, a[1]);

    // both sides do something
    n = 0; a[f()] += f(); printf("19: n=%d\n", n);
    // and one that does nothing stays as it was
    i = 5; i += 2; i *= i; printf("20: %d\n", i);
    return 0;
}

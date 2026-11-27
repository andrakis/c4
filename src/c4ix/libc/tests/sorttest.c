// sorttest.c: qsort keeps equal elements in their order, as glibc's does
// (test-libc-bare, against gcc's build)
#include <stdio.h>
#include <stdlib.h>

struct rec { short key; short id; };
struct odd { char k; char c[2]; };

static int by_key(const void *a, const void *b) { return ((const struct rec *)a)->key - ((const struct rec *)b)->key; }
static int by_k(const void *a, const void *b) { return ((const struct odd *)a)->k - ((const struct odd *)b)->k; }
static int by_int(const void *a, const void *b) { int x = *(const int *)a, y = *(const int *)b; return x < y ? -1 : x > y; }

static struct rec r[300];
static struct odd o[40];
static int v[1000];

int main() {
    int i, seed = 7, ok;
    for (i = 0; i < 300; i++) { seed = seed * 1103515245 + 12345; r[i].key = (seed >> 16) & 7; r[i].id = i; }
    qsort(r, 300, sizeof r[0], by_key);
    for (i = 0; i < 300; i++) printf("%d.%d%c", r[i].key, r[i].id, i % 15 == 14 ? '\n' : ' ');
    for (i = 0; i < 40; i++) { o[i].k = (char)(i * 7 % 5); o[i].c[0] = 'a' + i % 26; o[i].c[1] = 0; }
    qsort(o, 40, sizeof o[0], by_k);
    for (i = 0; i < 40; i++) printf("%d%s", o[i].k, o[i].c);
    printf("\n");
    for (i = 0; i < 1000; i++) { seed = seed * 1103515245 + 12345; v[i] = (seed >> 8) % 5000 - 2500; }
    qsort(v, 1000, sizeof v[0], by_int);
    ok = 1;
    for (i = 1; i < 1000; i++) if (v[i - 1] > v[i]) ok = 0;
    printf("ints sorted %d, %d .. %d\n", ok, v[0], v[999]);
    qsort(v, 1, sizeof v[0], by_int);
    qsort(v, 0, sizeof v[0], by_int);
    printf("done\n");
    return 0;
}

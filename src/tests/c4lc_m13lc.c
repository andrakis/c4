// c4lc_m13lc.c -- M13, steps 11-12 of docs/ports-feasibility.md §8, what
// Rise of the Triad needs:
//
//   - K&R definitions: int f(a, b) T *a; T b; { }, a parameter no
//     declaration names staying int, and an array parameter a pointer
//   - int + pointer: *(dest + bufferofs), dest the int
//   - a pointer to a function pointer: T (**pf)(args), stepped along a
//     table and called through
//   - a string that fills its array exactly, its NUL dropped, global and
//     local
//
// The golden (src/c4sp/tests/expected/c4lc-m13lc.txt) is gcc's, the same
// at 32 and 64 bits.

typedef struct { int height; char tag; } visobj;

int compareheights(s1p, s2p) visobj **s1p, **s2p;
{
    return (*s1p)->height - (*s2p)->height;
}

int sum3(a, b, c) char *a; int c;
{
    return a[0] + a[1] + b + c;
}

int first(v, n) short v[]; int n;
{
    return v[0] * 100 + v[n - 1];
}

static int twice(int x) { return 2 * x; }
static int thrice(int x) { return 3 * x; }
static int (*table[3])(int) = { twice, thrice, twice };

char savename[13] = "rottgam?.rot\0";
char exact[4] = "abcd";

int main() {
    visobj a, b, *pa, *pb;
    int (**pf)(int);
    unsigned char screen[16];
    int i, bufferofs, dest;
    char local[5] = "hello";
    short v[3];

    a.height = 70; b.height = 52;
    pa = &a; pb = &b;
    printf("knr %d %d %d\n", compareheights(&pa, &pb), sum3("AB", 1, 2), (v[0] = 7, v[2] = 9, first(v, 3)));

    for (i = 0; i < 16; i++) screen[i] = 0;
    dest = 3;
    bufferofs = (int) 0;
    {
        unsigned char *base = screen;
        *(dest + base) = 42;
        *(5 + base + 1) = 43;
    }
    printf("intptr %d %d %d\n", screen[3], screen[6], (int) ((2 + screen) - screen));

    pf = &table[0];
    printf("pfp %d", (*pf)(10));
    pf++;
    printf(" %d", (*pf)(10));
    printf(" %d\n", pf[1](10));

    printf("exact %d %c %c %c %d\n", (int) sizeof savename, savename[7], savename[11], exact[3], (int) sizeof exact);
    printf("local %c%c %d\n", local[0], local[4], (int) sizeof local);
    return 0;
}

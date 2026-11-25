// c4lc_ll.c -- long long (M8): 64 bits on a 32-bit target as well as on a
// 64-bit one. Compared with gcc's output at both sizes (the golden is
// gcc's). Printed through its own decimal routine, which divides a long
// long by ten until nothing is left, rather than printf's %lld.

long long g1 = 5000000000LL;
long long g2;
unsigned long long gu = 0xFFFFFFFFFFFFFFFFULL;
long long garr[3] = { 1, -2, 3000000000LL };
struct acc { int id; long long cycles; long long sys; };
struct acc gacc = { 7, 123456789012LL, -5 };

void pull(unsigned long long u) {
    char buf[24];
    int i;
    i = 23; buf[i] = 0;
    if (u == 0) { i = i - 1; buf[i] = '0'; }
    while (u != 0) { i = i - 1; buf[i] = '0' + (int)(u % 10); u = u / 10; }
    printf("%s", buf + i);
}
void pll(long long v) {
    if (v < 0) { printf("-"); pull(0 - (unsigned long long)v); }
    else pull(v);
}
void say(char *what, long long v) { printf("%s ", what); pll(v); printf("\n"); }

long long add3(long long a, int b, long long c) { return a + b + c; }
long long fact(int n) { if (n <= 1) return 1; return n * fact(n - 1); }
unsigned long long umax() { return 0xFFFFFFFFFFFFFFFFULL; }
// (unsigned) cuts to 32 bits at both word sizes; c4's int is the word
int low(long long v) { return (int)(unsigned)v; }

int main(void) {
    long long a, b, x, y, *p;
    unsigned long long u;
    long long arr[4];
    struct acc ac;
    int i, n;

    say("g1", g1);
    say("g2", g2);
    printf("gu "); pull(gu); printf("\n");
    say("garr", garr[0] + garr[1] + garr[2]);
    say("gacc", gacc.cycles + gacc.sys + gacc.id);

    a = 3000000000LL; b = 7;
    say("add", a + b);
    say("sub", a - b);
    say("mul", a * b);
    say("div", a / b);
    say("mod", a % b);
    say("neg", -a);
    say("not", ~a);
    say("and", a & 0xFFFF0000LL);
    say("or", a | 1);
    say("xor", a ^ 0x5555555555LL);
    say("bigmul", 123456789LL * 987654321LL);
    say("bigdiv", 121932631112635269LL / 987654321LL);
    say("bigmod", 121932631112635269LL % 1000000007LL);
    say("sdiv", -7LL / 2);
    say("smod", -7LL % 2);
    say("sdiv2", 7LL / -2);
    u = umax();
    printf("udiv "); pull(u / 3); printf("\n");
    printf("umod "); pull(u % 10); printf("\n");
    printf("ucmp %d %d\n", (unsigned long long)-1 > 1ULL, (long long)-1 < 1LL);

    say("shl", 1LL << 40);
    say("sar", -1099511627776LL >> 3);
    printf("shr "); pull(u >> 60); printf("\n");
    n = 33;
    say("shlv", 3LL << n);
    say("sarv", (-3LL << n) >> n);

    i = -5;
    x = i;
    say("widen", x);
    x = 4000000000u;
    say("widenu", x);
    say("narrow", low((1LL << 32) + 5));
    i = (int)(-(1LL << 33) - 9);
    printf("narrow2 %d\n", i);

    x = 5000000000LL; y = 5000000001LL;
    printf("cmp %d %d %d %d %d %d\n", x == y, x != y, x < y, x <= y, x > y, x >= y);
    printf("mixed %d %d\n", x > 7, 7 < x);

    x = 0xFFFFFFFFLL;
    say("post", x++);
    say("after", x);
    say("pre", ++x);
    say("dec", --x);
    x += 10; x *= 3; x -= 1; x /= 2; x <<= 1; x >>= 2;
    say("compound", x);

    say("add3", add3(10000000000LL, -1, 2));
    say("fact20", fact(20));

    ac.id = 1; ac.cycles = 10000000000LL; ac.sys = 2;
    ac.cycles += ac.sys;
    say("member", ac.cycles);
    arr[0] = 1; arr[1] = 1LL << 35; arr[2] = -3; arr[3] = 4;
    x = 0; i = 0;
    while (i < 4) { x = x + arr[i]; i = i + 1; }
    say("array", x);
    p = &arr[1];
    *p = *p + 1;
    p[1] += 10;
    say("ptr", arr[1] + arr[2]);

    x = 1LL << 40;
    if (x) printf("truth yes\n");
    printf("lognot %d %d\n", !x, !(x - x));
    // c4's && and || give an operand's value, not 0 or 1: compare it
    printf("land %d %d\n", (x && 0) != 0, (x || 0) != 0);
    y = x > 0 ? x : 5;
    say("cond", y);
    say("cond2", x < 0 ? 1 : x * 2);

    // what the CPU accounting does: big counters, a percentage of them
    x = 9876543210LL; y = 3 * x / 4;
    printf("pct %d\n", (int)(y * 100 / x));
    printf("size %d\n", (int)sizeof(long long));
    return 0;
}

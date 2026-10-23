// c4lc_m12lc.c -- M12, steps 8-10 of docs/ports-feasibility.md §8, what
// OpenTyrian needs, which only c4lc takes (c4fc has no typedef):
//
//   - declarations in for: for (int i = 0, j = 2; ...), shadowing an outer
//     name, nested, and the outer name back after the loop
//   - a struct returned by value and used: to initialize, in an assignment,
//     a member of the call, as an argument to a struct parameter, returned
//     again, and from a recursive function (through its caller's pointer,
//     so each call has its own)
//   - a struct initialized from a struct: a variable, a member, a call
//   - compound literals: a struct one returned (OpenTyrian's MemReader), an
//     array one passed (its demo.c), one in a loop made afresh each time
//   - sizeof of a local array in an array size (its menus.c)
//   - a local initialized with a float on one side, converted as an
//     assignment converts (its JE_gammaCorrect_func: int temp = *col * r)
//
// The golden (src/c4sp/tests/expected/c4lc-m12lc.txt) is gcc's, the same
// at 32 and 64 bits.

typedef struct { short a, b; } P;
typedef struct { char *data; short size; short err; } MemReader;
typedef struct { P lo, hi; } Box;

P mk(int x) {
    P p;
    p.a = (short) x;
    p.b = (short) (x * 2);
    return p;
}

P add(P x, P y) {
    P r;
    r.a = (short) (x.a + y.a);
    r.b = (short) (x.b + y.b);
    return r;
}

// returned again: the callee's result is the caller's own return value
P twice(int x) {
    return add(mk(x), mk(x));
}

// recursion: each call's value lives in its own caller's temporary
P fib(int n) {
    P r, s;
    if (n < 2) return mk(n);
    r = fib(n - 1);
    s = fib(n - 2);
    return add(r, s);
}

MemReader reader(char *data, int size) {
    if (!data) return (MemReader) { 0, 0, 1 };
    return (MemReader) { data, (short) size, 0 };
}

Box box(int x) {
    Box b;
    b.lo = mk(x);
    b.hi = mk(x + 1);
    return b;
}

// int from float, float and double from int, in an initializer
static int gamma1(unsigned char *col, float r) {
    int temp = *col * r;
    float f = *col;
    double d = temp;
    long long ll = r * 1000;
    float g = ll;
    return temp + (int) (f * 2) + (int) (d / 2) + (int) (g / 100);
}

int sum(unsigned char *p, int n) {
    int s = 0;
    while (n--) s += *p++;
    return s;
}

P gp = { 7, 8 };

int main() {
    int i, s, menus[5];
    int parents[sizeof(menus) / sizeof *(menus)];
    P q = mk(3);
    P r = q;
    P t = gp;
    P u;
    Box bx = box(4);
    P lo = bx.lo;
    MemReader m, z;

    // for
    s = 0;
    i = 100;
    for (int i = 0; i < 4; i++) s += i;
    for (int i = 0, j = 2; i < j; ++i) s += j * 10;
    for (int k = 0; k < 3; k++)
        for (int i = 0; i < k; i++) s += 100;
    printf("for %d %d\n", s, i);

    // struct values
    printf("init %d %d / %d %d / %d %d\n", q.a, q.b, r.a, r.b, t.a, t.b);
    u = mk(5);
    printf("assign %d %d\n", u.a, u.b);
    printf("member %d %d\n", mk(6).b, box(9).hi.a);
    u = add(mk(1), q);
    printf("arg %d %d\n", u.a, u.b);
    u = twice(4);
    printf("twice %d %d\n", u.a, u.b);
    u = fib(10);
    printf("fib %d %d\n", u.a, u.b);
    printf("box %d %d %d %d, lo %d %d\n", bx.lo.a, bx.lo.b, bx.hi.a, bx.hi.b, lo.a, lo.b);

    // compound literals
    m = reader("hello", 5);
    z = reader(0, 9);
    printf("reader %c %d %d / %d %d\n", m.data[1], m.size, m.err, z.size, z.err);
    printf("arr %d\n", sum((unsigned char[3]) { 1, 2, 3 }, 3));
    s = 0;
    for (i = 0; i < 4; i++) {
        P *pp = &(P) { (short) i, (short) (i * i) };
        s += pp->a + pp->b;
        pp->b = 99;
    }
    printf("loop %d\n", s);

    // initializers that convert
    {
        unsigned char c = 100;
        int k = 1.5f * 3;
        float h = k;
        printf("convert %d %d %d\n", gamma1(&c, 1.5f), k, (int) (h * 10));
    }

    // sizeof a local in a constant
    printf("sizeof %d\n", (int) (sizeof(parents) / sizeof(parents[0])));
    return 0;
}

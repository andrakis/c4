// M7b: the C that Busybox needs and c4lc did not take (docs/busybox-port.md).
// gcc wrote src/c4sp/tests/expected/c4lc-m7.txt; c4lc must print the same
// at 32 and 64 bits, and c4fc must build the same bytes (C4FC_GEN_SWEEP).
// Each step of M7b adds its constructs here. int is a whole word in c4, so
// sizes are compared rather than printed.

// (void) is no parameters; a prototype's parameters may have no names
int seven(void) { return 7; }
int add(int, int);
int add(int a, int b) { return a + b; }
char *pick(char *, int);
char *pick(char *s, int i) { return s + i; }
void nothing(void) { }
int is_set(void *p) { return p != 0; }
static int counter(void);
static int counter(void) { return 41 + 1; }

// sizeof of any expression, with or without parentheses
struct rec { int id; char name[6]; short tag; };
struct rec grec;
char gbuf[10];
int garr[4];
int sizes(struct rec *pr) {
    int a; char c; short s; long l; char *p; int arr[3];
    a = 0; c = 0; s = 0; l = 0; p = 0; arr[0] = 0;
    printf("sizeof-scalar %d %d %d %d %d\n", sizeof a == sizeof(int), sizeof c == 1,
           sizeof(s) == 2, sizeof l == sizeof(long), sizeof(p) == sizeof(char *));
    printf("sizeof-array %d %d %d %d\n", sizeof gbuf == 10, sizeof(garr) == 4 * sizeof(int),
           sizeof arr == 3 * sizeof(int), sizeof(arr[0]) == sizeof(int));
    printf("sizeof-member %d %d %d %d\n", sizeof(grec.name) == 6, sizeof pr->name == 6,
           sizeof(pr->tag) == 2, sizeof(grec.id) == sizeof(int));
    printf("sizeof-struct %d %d\n", sizeof(*pr) == sizeof(struct rec), sizeof grec == sizeof(struct rec));
    printf("sizeof-expr %d %d %d\n", sizeof(a + 1) == sizeof(int), sizeof("abc") == 4,
           sizeof(char[7]) == 7);
    // BUILD_BUG_ON, as Busybox writes it
    ((void)sizeof(char[1 - 2*!!(sizeof(c) != 1)]));
    return 0;
}

// sizeof, comparisons, && || and ?: in constant expressions
char cbuf[sizeof("hello")];
int carr[sizeof(short) * 3 + 2];
enum { E_ONE = sizeof(char) == 1 ? 1 : -1, E_AND = (3 > 2) && (1 || 0), E_LT = 2 < 1,
       E_LE = 2 <= 2, E_NE = 5 != 5, E_COND = 0 ? 10 : 20 };
struct BUG_check { char ok[sizeof(int) >= 4 ? 1 : -1]; };

// function pointers (M7b): declarators for locals, globals, arrays,
// parameters and struct members, and calls through any expression
int twice(int x) { return 2 * x; }
int negate(int x) { return -x; }
int apply(int (*f)(int), int v) { return f(v); }
struct op { char *name; int (*fn)(int); };
int (*gops[2])(int);
int (*gone)(int);
int fnptrs(void) {
    int (*fp)(int);
    struct op o; struct op *po;
    int (*lops[2])(int);
    fp = twice;
    o.name = "neg"; o.fn = negate; po = &o;
    gops[0] = twice; gops[1] = negate; gone = negate;
    lops[0] = negate; lops[1] = twice;
    printf("fnptr %d %d %d %d\n", fp(21), (*fp)(4), apply(negate, 5), apply(fp, 6));
    printf("fnptr-member %d %d %s\n", o.fn(7), po->fn(8), po->name);
    printf("fnptr-array %d %d %d %d\n", gops[0](3), gops[1](3), lops[0](9), gone(2));
    printf("fnptr-cast %d\n", ((int (*)(int))fp)(10));
    printf("fnptr-sizeof %d\n", sizeof(fp) == sizeof(char *));
    return 0;
}

// goto and labels (M7b): backward, forward, out of nested loops, into a
// branch -O must not drop, and labels before `;` and before `}`
int gotos(int n) {
    int i; int s;
    i = 0; s = 0;
again:
    s = s + i;
    i++;
    if (i < n) goto again;
    if (s > 100) goto big;
    goto done;
big:
    s = 100;
done:
    return s;
}
int nested_out(void) {
    int i; int j; int hit;
    hit = -1;
    for (i = 0; i < 5; i++)
        for (j = 0; j < 5; j++)
            if (i * j == 6) { hit = i * 10 + j; goto out; }
out:
    return hit;
}
int dead_branch(void) {
    int r;
    r = 1;
    goto inside;
    if (0) {
inside:
        r = 7;
    }
    return r;
}
int label_at_end(int x) {
    if (x) goto end;
    x = 5;
end:
    ;
    return x;
}
void label_last(int *p) {
    if (*p > 3) goto skip;
    *p = 3;
skip:
}
int labels(void) {
    int a; int b;
    a = 5; b = 1;
    label_last(&a); label_last(&b);
    printf("goto %d %d %d %d\n", gotos(5), gotos(20), nested_out(), dead_branch());
    printf("goto-end %d %d %d %d\n", label_at_end(0), label_at_end(1), a, b);
    return 0;
}

// enum as a type anywhere (M7b): a global, a local, a member, a
// parameter, a cast, sizeof, and an enum with a body inside a function
enum colour { RED, GREEN = 5, BLUE };
enum colour gcol;
struct paint { enum colour c; int n; };
int shade(enum colour c) { return c * 10; }
int enums(void) {
    enum colour c; struct paint p; enum { LOCAL_A = 3, LOCAL_B } lv;
    c = BLUE; gcol = GREEN; p.c = RED; p.n = 1; lv = LOCAL_B;
    printf("enum %d %d %d %d %d %d\n", c, gcol, p.c, shade(BLUE), lv, (int)(enum colour)2);
    printf("enum-size %d %d\n", sizeof(enum colour) == sizeof(int), sizeof c == sizeof(int));
    return 0;
}

int main(void) {
    nothing();
    printf("void-params %d %d %s\n", seven(), add(2, 3), pick("xyz", 1));
    printf("void-ptr %d %d\n", is_set(0), is_set("x"));
    printf("static-void %d\n", counter());
    sizes(&grec);
    printf("const-sizeof %d %d\n", sizeof cbuf == 6, sizeof carr == 8 * sizeof(int));
    printf("const-ops %d %d %d %d %d %d\n", E_ONE, E_AND, E_LT, E_LE, E_NE, E_COND);
    fnptrs();
    labels();
    enums();
    return 0;
}

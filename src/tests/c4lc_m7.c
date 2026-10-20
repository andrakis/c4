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

// static locals (M7b): a global named FUNCTION.NAME. Each keeps its value
// between calls, two functions' `n` are two objects, and neither is the
// global `n`. A function nothing calls takes its static with it under -O.
int n = 1000;
int bump(void) { static int n = 10; n = n + 1; return n; }
int tally(int k) { static int n; static char tag[4] = "ab"; n = n + k; tag[2] = 'c'; return n * 100 + tag[2]; }
char *greeting(void) { static char *s = "hi"; return s; }
int never_called(void) { static int unused = 5; return unused; }
int statics(void) {
    int i; int last;
    for (i = 0; i < 3; i++) last = bump();
    tally(2);
    printf("static %d %d %d %s\n", last, tally(5), n, greeting());
    return 0;
}

// aggregate initializers (M7b): an applet-style table of structs with
// strings and function addresses, arrays of strings and of function
// pointers, struct globals, nested structs and char-array members,
// [i] = and .m = designators, brace elision, a union, and the old
// plain-numbers path beside them
int a_main(int c) { return 10 + c; }
int b_main(int c) { return 20 + c; }
struct applet { char *name; int (*main)(int); short flags; char tag; };
static struct applet applets[] = {
    { "a", a_main, 1, 'x' },
    { "b", b_main, 2, 'y' },
};
char *words[] = { "one", "two", "three" };
int (*mains[])(int) = { a_main, b_main };
struct pt { int x; int y; } origin = { 3, 4 };
struct pt pts[3] = { { 1, 2 }, [2] = { 5, 6 } };
int sparse[6] = { [1] = 7, [4] = 9, 10 };
struct box { char label[6]; struct pt corner; int w; } box1 = { "lid", { 7, 8 }, 9 };
struct box box2 = { .w = 11, .label = "ab" };
struct pt elided[2] = { 1, 2, 3, 4 };
unsigned char bytes[] = { 1, 200, 3 };
short halves[3] = { -1, 300 };
union num { int i; char c; } u1 = { 65 };
int aggs(void) {
    int i; int s;
    s = 0;
    for (i = 0; i < 2; i++) s = s + applets[i].main(i) + applets[i].flags + applets[i].tag;
    printf("agg-table %s %s %d %d\n", applets[0].name, applets[1].name, s,
           (int)(sizeof(applets) / sizeof(applets[0])));
    printf("agg-words %s %s %s %d\n", words[0], words[1], words[2],
           (int)(sizeof(words) / sizeof(words[0])));
    printf("agg-mains %d %d\n", mains[0](1), mains[1](2));
    printf("agg-struct %d %d %d %d %d %d\n", origin.x, origin.y, pts[0].y, pts[1].x, pts[2].x, pts[2].y);
    printf("agg-sparse %d %d %d %d %d %d\n", sparse[0], sparse[1], sparse[2], sparse[4], sparse[5],
           (int)(sizeof(sparse) / sizeof(sparse[0])));
    printf("agg-nested %s %d %d %d\n", box1.label, box1.corner.x, box1.corner.y, box1.w);
    printf("agg-desig %s %d %d\n", box2.label, box2.corner.x, box2.w);
    printf("agg-elided %d %d %d %d\n", elided[0].x, elided[0].y, elided[1].x, elided[1].y);
    printf("agg-small %d %d %d %d %d\n", bytes[1], halves[0], halves[1], halves[2], u1.i & 255);
    return 0;
}

// extern T x[] (M7b): declared without a size, used, then defined; and a
// union read through its other member (c4fc laid unions out as structs)
extern const char banner[];
extern int table[];
int ext_first(void) { return banner[1] + table[2]; }
const char banner[] = "hey";
int table[] = { 4, 5, 6 };
int externs(void) {
    printf("extern %d %s %d\n", ext_first(), banner, (int)(sizeof(table) / sizeof(table[0])));
    printf("union %d %d\n", u1.c, sizeof(union num) == sizeof(int));
    return 0;
}

// an enum constant whose value is 0, in a brace list (the first cut took
// RED for a function name)
int etab[3] = { BLUE, RED, GREEN };
struct ecol { enum colour c; char *n; } ecols[] = { { RED, "red" }, { BLUE, "blue" } };
int enum0(void) {
    printf("enum0 %d %d %d %d %s\n", etab[0], etab[1], etab[2], ecols[1].c, ecols[0].n);
    return 0;
}

// inline, restrict, __extension__ and __attribute__ (M7b): dropped
// before parsing, as Busybox's platform.h writes them on nearly every
// prototype; constructor and destructor still mean what they did
static inline int sq(int x) { return x * x; }
static __inline__ int cube(int x) { return x * sq(x); }
static int noisy(int x) __attribute__((unused));
static int __attribute__((noinline)) noisy(int x) { return x + 1; }
int fmt(const char *f) __attribute__ ((nonnull (1), warn_unused_result));
int fmt(const char *f) { return f[0]; }
struct __attribute__((packed)) tight { char a; int b; } __attribute__((aligned(8)));
int copy3(char *restrict d, const char *restrict s) { d[0] = s[0]; d[1] = s[1]; d[2] = 0; return 2; }
__extension__ int ext_word;
int attrs(void) {
    char buf[4];
    struct tight t;
    t.a = 1; t.b = 2;
    copy3(buf, "xy");
    printf("attr %d %d %d %d %s %d\n", sq(5), cube(2), noisy(1), fmt("q"), buf, t.a + t.b);
    return 0;
}

// struct assignment (M7b): a whole struct copied, as libbb's
// `*ptm = *lt;` and hush's `*ctx = *old;` do -- variables, through
// pointers, members, array elements, and a struct holding an array
struct ctx { int depth; char name[6]; short flags; struct pt where; };
struct ctx gctx = { 3, "gl", 7, { 1, 2 } };
struct ctx ctxs[2];
int scopy(void) {
    struct ctx a; struct ctx b; struct ctx *pa; struct ctx *pb;
    a.depth = 5; a.name[0] = 'h'; a.name[1] = 'i'; a.name[2] = 0; a.flags = -2; a.where.x = 8; a.where.y = 9;
    b = a;
    pa = &a; pb = &ctxs[1];
    *pb = *pa;
    ctxs[0] = gctx;
    a.where = gctx.where;
    printf("scopy %d %s %d %d %d\n", b.depth, b.name, b.flags, b.where.x, b.where.y);
    printf("scopy2 %d %s %d %s %d %d\n", ctxs[1].depth, ctxs[1].name, ctxs[0].depth, ctxs[0].name, a.where.x, a.where.y);
    return 0;
}

// __func__ (M7b): the enclosing function's name, as a string
char *whoami(void) { return __func__; }
int funcname(void) {
    printf("func %s %s %d\n", whoami(), __func__, (int)sizeof(__func__));
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
    statics();
    aggs();
    externs();
    enum0();
    attrs();
    scopy();
    funcname();
    return 0;
}

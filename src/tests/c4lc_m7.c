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

int main(void) {
    nothing();
    printf("void-params %d %d %s\n", seven(), add(2, 3), pick("xyz", 1));
    printf("void-ptr %d %d\n", is_set(0), is_set("x"));
    printf("static-void %d\n", counter());
    sizes(&grec);
    printf("const-sizeof %d %d\n", sizeof cbuf == 6, sizeof carr == 8 * sizeof(int));
    printf("const-ops %d %d %d %d %d %d\n", E_ONE, E_AND, E_LT, E_LE, E_NE, E_COND);
    return 0;
}

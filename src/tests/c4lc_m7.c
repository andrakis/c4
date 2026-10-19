// M7b: the C that Busybox needs and c4lc did not take (docs/busybox-port.md).
// gcc wrote src/c4sp/tests/expected/c4lc-m7.txt; c4lc must print the same
// at 32 and 64 bits, and c4fc must build the same bytes (C4FC_GEN_SWEEP).
// Each step of M7b adds its constructs here.

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

int main(void) {
    nothing();
    printf("void-params %d %d %s\n", seven(), add(2, 3), pick("xyz", 1));
    printf("void-ptr %d %d\n", is_set(0), is_set("x"));
    printf("static-void %d\n", counter());
    return 0;
}

#define __attribute__(x)
#define FAST_FUNC __attribute__((regparm(3)))
static int FAST_FUNC f(int x) __attribute__((unused));
static int FAST_FUNC f(int x) { return x; }
int main() { printf("ama %d\n", f(3)); return 0; }

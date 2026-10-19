static int f(int x) __attribute__((unused));
static int f(int x) { return x; }
struct __attribute__((packed)) q { char a; int b; };
int __attribute__((noinline)) g(void) { return 7; }
int main() { printf("attr %d %d\n", f(3), g()); return 0; }

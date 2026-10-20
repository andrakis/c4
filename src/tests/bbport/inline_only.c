// static: a plain C99 inline has no external definition, so gcc cannot link it
static inline int sq(int x) { return x * x; }
int main() { printf("io %d\n", sq(6)); return 0; }

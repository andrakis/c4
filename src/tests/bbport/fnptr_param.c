int apply(int (*f)(int), int v) { return f(v); }
int inc(int x) { return x + 1; }
int main() { printf("param %d\n", apply(inc, 41)); return 0; }

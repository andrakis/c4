int f(int x) { return x + 1; }
int main() { void *v; int (*g)(int); v = (void *)f; g = (int (*)(int))v; printf("cfp %d\n", g(1)); return 0; }

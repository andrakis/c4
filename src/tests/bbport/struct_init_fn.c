int m(int x) { return x + 1; }
struct ap { int f; int fn; };
struct ap a[1] = { { 1, m } };
int main() { int g; g = a[0].fn; printf("sif %d\n", g(1)); return 0; }

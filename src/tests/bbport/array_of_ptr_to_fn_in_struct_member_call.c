struct ops { int (*f[2])(int); };
int a(int x) { return x; } int b(int x) { return -x; }
struct ops o = { { a, b } };
int main() { printf("afs %d\n", o.f[1](5)); return 0; }

int a(int x) { return x; } int b(int x) { return -x; }
int (*tbl[2])(int) = { a, b };
int main() { int (*f)(int); f = tbl[1]; printf("fat %d\n", f(5)); return 0; }

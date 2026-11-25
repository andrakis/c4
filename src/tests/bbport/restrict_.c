int f(char *restrict a, const char *restrict b) { return a[0] + b[0]; }
int main() { printf("rs %d\n", f("a", "b")); return 0; }

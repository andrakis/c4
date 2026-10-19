int main() { int x; x = 3; if (__builtin_expect(x == 3, 1)) printf("be ok\n"); return 0; }

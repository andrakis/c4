int main() { int a; int b; a = (b = 3, b + 1); printf("tc %d %d\n", a > 3 ? a : b, a); return 0; }

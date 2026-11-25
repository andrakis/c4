union u { int i; char c[4]; };
int main() { union u x; x.i = 0; x.c[0] = 65; printf("union %d\n", x.i & 255); return 0; }

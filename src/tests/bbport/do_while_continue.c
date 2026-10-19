int main() { int i; int s; i = 0; s = 0; do { i++; if (i & 1) continue; s += i; } while (i < 10); printf("dw %d\n", s); return 0; }

int f(int n) { char b[n]; int i; for (i = 0; i < n; i++) b[i] = i; return b[n - 1]; }
int main() { printf("vla %d\n", f(9)); return 0; }

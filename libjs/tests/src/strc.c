// strc.c -- STRC names the functions on the stack, innermost first, from
// the image's own symbol section (libjs boot.js parseSymbols).
int inner (int n) { if (n) return inner(n - 1); stacktrace(); return 7; }
int middle () { return inner(2); }
int main () { printf("result %d\n", middle()); return 0; }

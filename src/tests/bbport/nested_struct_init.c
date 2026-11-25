struct in { int a; const char *s; };
struct out { int n; struct in i[2]; };
struct out o = { 2, { { 1, "x" }, { 2, "y" } } };
int main() { printf("nsi %d %d %s\n", o.n, o.i[1].a, o.i[1].s); return 0; }

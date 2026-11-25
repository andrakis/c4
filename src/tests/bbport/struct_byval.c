struct s { int a; int b; };
int sum(struct s v) { return v.a + v.b; }
struct s mk(int a) { struct s r; r.a = a; r.b = a * 2; return r; }
int main() { struct s t; t = mk(5); printf("byval %d %d\n", sum(t), t.b); return 0; }

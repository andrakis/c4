// c4lc_m4e.c -- M4: const, the part that must NOT compile.
//
// Compiled once per case with -P -D CASE=n; each case is one write the
// type system forbids, and c4lc and c4fc must refuse it with the same
// line (src/c4sp/tests/expected/c4lc-m4e-diag.txt).

struct S { const int a; int b; };
const int g = 3;
const int arr[3] = { 1, 2, 3 };

int main() {
	const int x = 1;
	const char *p = "a";
	char buf[4];
	char * const fixed = buf;
	struct S s;
	const struct S *sp = &s;
	int i = 0;
#if CASE == 1
	x = 2;
#elif CASE == 2
	*p = 'b';
#elif CASE == 3
	g++;
#elif CASE == 4
	s.a--;
#elif CASE == 5
	fixed = 0;
#elif CASE == 6
	sp->b = 1;
#elif CASE == 7
	arr[1] = 5;
#elif CASE == 8
	--x;
#elif CASE == 9
	p[i] = 'c';
#endif
	return 0;
}

// c4lc_m4q.c -- M4: const and volatile, the part that compiles.
//
// Every way of dropping a const without a cast warns -- assignment,
// initialisation, an argument, a return, and two levels down -- and
// every way of keeping it, or of dropping it WITH a cast, does not. The
// warnings are pinned in src/c4sp/tests/expected/c4lc-m4q-diag.txt and
// c4lc and c4fc must print exactly those lines. The program then runs:
// a static const is a constant wherever C asks for one, and keeps its
// storage. (c4lc_m4e.c is the half that must NOT compile.)

static const int N = 3;
static const int M = N * 4 + 1;
static const unsigned char UC = 300;
const char *greeting = "hello";
volatile int ticks;
int table[M];
signed char sc = -1;

void takes(char *s) { printf("takes %s\n", s); }
void keeps(const char *s) { printf("keeps %s\n", s); }
char *unconst(const char *s) { return s; }
const char *same(const char *s) { return s; }

int pick(int k) {
	switch (k) {
	case N: return 30;
	case M: return 130;
	case UC: return 440;
	default: return -1;
	}
}

int main() {
	const char *cp;
	char *p;
	char buf[8];
	char *q = greeting;
	const char *ok = greeting;
	const char **cpp;
	char **pp;
	char * const fixed = buf;
	volatile const int vc = 9;
	int i;
	cp = "const";
	p = cp;
	p = (char *)cp;
	cp = p;
	takes(cp);
	takes((char *)cp);
	keeps(cp);
	keeps(p);
	p = unconst(cp);
	p = same(cp);
	cp = same(p);
	cpp = &cp;
	pp = cpp;
	pp = (char **)cpp;
	*fixed = 'x';
	fixed[1] = 0;
	ticks = 1;
	ticks = ticks + vc;
	printf("%d %d %d %d\n", pick(3), pick(13), pick(44), pick(5));
	printf("%d %d %d %d\n", N, M, UC, (int)(sizeof(table) / sizeof(int)));
	printf("%s %s %s %d %d\n", q, ok, buf, ticks, sc);
	return 0;
}

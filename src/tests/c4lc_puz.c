// What Simon Tatham's Puzzles needed of c4lc: _Bool, pointers to functions
// returning float or double, a variable named like a typedef, a tag defined
// again in a function, (*(fns[])), a whole struct in a local aggregate's
// initializer. gcc wrote src/c4sp/tests/expected/c4lc-puz.txt, and c4lc must
// print the same, at 32 and 64 bits, plain, -O and -minline. c4lc only: c4fc
// refuses float, and these came after it.
#include <stdio.h>
#include <string.h>

struct S { _Bool a; _Bool b; char c; int d; _Bool e[3]; };
typedef float (*ff_t)(int);
struct G { float (*fl)(int); double (*dl)(int, int); int (*il)(int); ff_t tf; };
static float half(int x) { return x / 2.0F; }
static double third(int x, int y) { return (x + y) / 3.0; }
static int twice(int x) { return x * 2; }
static _Bool isb(int x) { return x & 256; }
static _Bool takes(_Bool b) { return b; }
static double (*gd)(int, int) = third;
static float (*tab[2])(int) = { half, half };
static const struct G g = { half, third, twice, half };
static _Bool gb[4] = { 1, 0, 1 };
static int redraw(int a, float t, float f) { return a + (int)(t * 10) + (int)(f * 100); }
static int (*rd)(int, float, float) = redraw;
static int inc(int x) { return x + 1; }
static int dec(int x) { return x - 1; }
static int (*(fns[]))(int) = { inc, dec };

// the name is the variable's, to the end of its block
typedef struct node { int v; struct node *next; } node;
static int shadowed(node *node) {
	int r = node->v;
	if (node) r++;
	r += (node) ? 10 : 20;
	return r;
}
static int shadow2(int k) {
	node n1;
	n1.v = k;
	{
		node *node = &n1;
		node->v++;
		if (k > 100) return (node) ? 1 : 0;
	}
	{
		node *m = &n1;      /* and the type's again */
		return m->v;
	}
}

// the same tag, another struct in here
struct grid { int a; int b; int c; };
static int local_tag(int k) {
	struct grid { char t; } l;
	l.t = k;
	return sizeof(struct grid) + l.t;
}
static int after_tag(void) {
	struct grid x;
	x.c = 3;
	return x.c + (int)(sizeof(struct grid) / sizeof(int));
}
// a whole struct as an element, by name, through a pointer and as a member
struct P { int x; int y; };
struct T { struct P v[3]; };
static int struct_elems(const struct T *t) {
	struct P a = { 5, 6 };
	struct P v[4] = { t->v[1], a, t->v[0], };
	struct P w[2] = { { 1, 2 }, v[1] };
	return v[0].x * 1000 + v[1].y * 100 + v[2].x * 10 + v[3].x + w[1].x + w[0].y;
}

int main(void) {
	_Bool b = 256;
	_Bool c = 0.5F;
	_Bool e = 0.0;
	struct S s;
	long long ll = 1LL << 40;
	_Bool *p;
	float (*lf)(int) = half;
	float (**pf)(int) = &lf;
	float x;
	double d;
	int i = 512;
	struct T t = { { { 1, 2 }, { 3, 4 }, { 7, 8 } } };
	memset(&s, 0, sizeof(s));
	printf("size %d\n", (int)sizeof(_Bool));
	printf("%d %d %d\n", b, c, e);
	s.a = i; s.b = i & 1; s.e[1] = i | 3; s.c = 'x'; s.d = 7;
	printf("%d %d %d %c %d\n", s.a, s.b, s.e[1], s.c, s.d);
	p = s.e;
	p[2] = 1024;
	p[0] = (_Bool)ll;
	printf("%d %d %d %d\n", s.e[0], s.e[1], s.e[2], (int)(p + 1 - s.e));
	printf("%d %d %d\n", isb(256), isb(255), takes(4096));
	printf("%d %d\n", (_Bool)4096, (_Bool)0.0);
	b = 0; b |= i & 512; printf("%d\n", b);
	b = 1; b &= (i & 1) == 0; printf("%d\n", b);
	printf("%d\n", gb[0] + gb[1] + gb[2] + gb[3]);
	x = g.fl(5); printf("%d\n", (int)(x * 1000));
	d = g.dl(1, 2); printf("%d\n", (int)(d * 1000));
	printf("%d %d\n", g.il(21), (int)(g.tf(9) * 1000));
	x = lf(7); printf("%d\n", (int)(x * 1000));
	x = (*pf)(3); printf("%d\n", (int)(x * 1000));
	x = tab[1](11); printf("%d\n", (int)(x * 1000));
	d = gd(4, 5); printf("%d\n", (int)(d * 1000));
	d = (*g.dl)(10, 11) + 1.0; printf("%d\n", (int)(d * 1000));
	printf("%d\n", (int)((g.fl(1) + g.fl(3)) * 1000));
	if (g.fl(0)) puts("no"); else puts("zero");
	printf("%d %d\n", rd(1, 0.5F, 0.25F), rd(1, x, x) > 0);
	printf("%d\n", g.fl != 0);
	// a double for a float parameter, and an int: converted, as a prototype says
	printf("%d %d\n", rd(1, 0.5, 0.25), rd(2, 1, 0));
	printf("%d %d\n", fns[0](4), fns[1](4));
	{
		node n; n.v = 4;
		printf("%d %d\n", shadowed(&n), shadow2(9));
	}
	printf("%d %d\n", local_tag(7), after_tag());
	printf("%d\n", struct_elems(&t));
	return 0;
}

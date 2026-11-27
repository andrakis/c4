// r_argorder.cpp: -fargs-rtl, a call's arguments evaluated in g++ -m32's
// order (right to left), as Blood's random spreads need (actFireVector)
extern "C" int printf (const char *, ...);

static int n;
static int seed = 1;
static int r (int tag) { printf (" r%d", tag); return ++n; }
static int Random (int range) { seed = seed * 1103515245 + 12345; return ((seed >> 16) & 0x7fff) % range; }
static void f3 (int a, int b, int c) { printf (" -> %d %d %d\n", a, b, c); }
static void f5 (int a, int b, int c, int d, int e) { printf (" -> %d %d %d %d %d\n", a, b, c, d, e); }

struct P { int x, y; };
static P mk (int x, int y) { P p; p.x = x; p.y = y; return p; }
static void fp (P a, P b) { printf (" -> (%d %d) (%d %d)\n", a.x, a.y, b.x, b.y); }

struct S {
	int v;
	S () { v = 100; }
	void m (int a, int b) { printf (" -> m%d %d %d\n", v, a, b); }
	int bump () { return ++v; }
};

static int g;
static int setg (int x) { g = x; return x; }

int main ()
{
	S s;
	unsigned char c = 250;
	int i = 0, k[4] = { 10, 20, 30, 40 };
	const char *names[2] = { "a", "b" };
	f3 (r (1), r (2), r (3));
	f5 (0, 1 + r (1), 2 + r (2), r (3) * 2, 4);
	f3 (Random (1000), Random (1000), Random (500));
	s.m (r (1), r (2));
	s.m (s.bump (), s.bump ());
	f3 (g, setg (7), g);
	f3 (i++, i++, i);
	f3 (k[i], ++i, k[i]);
	f3 (c++, c++, c);
	fp (mk (r (1), r (2)), mk (r (3), r (4)));
	f3 (r (r (1) + 8), 5, r (6));
	printf ("%s %d %s %d\n", names[r (1) & 1], r (2), names[r (3) & 1], r (4));
	printf ("%d\n", n);
	return 0;
}

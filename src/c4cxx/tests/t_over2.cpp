// t_over2.cpp: qtools' Rect (overloaded constructors, a user's copy
// constructor, operator! and operator&=), an operator[] returning a
// reference (DUDEINFOACCESS[i].x = 1), qtools' Error built as a temporary
// for a handler called through a pointer, ++ before and after, ==
extern "C" int printf (const char *, ...);

typedef unsigned char QBOOL;

inline int ClipLow (int a, int b) { return a < b ? b : a; }
inline int ClipHigh (int a, int b) { return a > b ? b : a; }

class Rect {
public:
	int f_0;
	int f_4;
	int f_8;
	int f_c;

	Rect (int a1, int a2, int a3, int a4) : f_0 (a1), f_4 (a2), f_8 (a3), f_c (a4) { }
	Rect (Rect &other) { f_0 = other.f_0; f_4 = other.f_4; f_8 = other.f_8; f_c = other.f_c; copies++; }
	Rect () : f_0 (0), f_4 (0), f_8 (0), f_c (0) { }

	QBOOL isValid (void) const { return f_0 < f_8 && f_4 < f_c; }
	QBOOL isEmpty (void) const { return !isValid (); }
	QBOOL operator! (void) const { return isEmpty (); }

	Rect &operator&= (Rect &other)
	{
		f_0 = ClipLow (f_0, other.f_0);
		f_4 = ClipLow (f_4, other.f_4);
		f_8 = ClipHigh (f_8, other.f_8);
		f_c = ClipHigh (f_c, other.f_c);
		return *this;
	}
	bool operator== (Rect &other) { return f_0 == other.f_0 && f_4 == other.f_4 && f_8 == other.f_8 && f_c == other.f_c; }
	void show (const char *what) { printf ("%s %d %d %d %d\n", what, f_0, f_4, f_8, f_c); }
	static int copies;
};

int Rect::copies = 0;

struct DUDEINFO {
	int x;
	int health;
};

DUDEINFO dudeInfo[4];

class DUDEINFOACCESS {
public:
	DUDEINFO &operator[] (int nType) { return dudeInfo[nType - 10]; }
};

DUDEINFOACCESS gDudeInfo;

struct Error {
	const char *f_0;
	int f_4;
	const char *f_8;
	char f_c;
	char __f_d[7];
	Error (const char *m, int l, const char *s) { f_0 = m; f_4 = l; f_8 = s; f_c = 0; }
};

typedef void (*ErrorHandler) (const Error &);

void DefaultHandler (const Error &e) { printf ("error in %s at %d: %s %d\n", e.f_0, e.f_4, e.f_8, (int)sizeof e); }

ErrorHandler curHandler = DefaultHandler;

void raise (const char *module, int line, const char *s) { curHandler (Error (module, line, s)); }

struct Counter {
	int n;
	Counter () : n (0) {}
	Counter &operator++ () { n += 1; return *this; }
	Counter &operator++ (int) { n += 10; return *this; }
	int operator[] (int i) { return n * i; }
};

int main ()
{
	Rect a (0, 0, 100, 50), b (20, 10, 200, 40), e;
	Rect c = a;
	Rect d (b);
	printf ("copies %d\n", Rect::copies);
	printf ("valid %d %d empty %d %d\n", a.isValid (), e.isValid (), !a, !e);
	c &= b;
	c.show ("c");
	(d &= a) &= c;
	d.show ("d");
	printf ("eq %d %d\n", c == d, a == b);
	gDudeInfo[11].x = 1;
	gDudeInfo[12].health = 50;
	gDudeInfo[12].health += 7;
	printf ("dude %d %d %d\n", dudeInfo[1].x, dudeInfo[2].health, gDudeInfo[11].x);
	raise ("error.cpp", 63, "something");
	Counter k;
	++k;
	k++;
	++k;
	printf ("counter %d %d\n", k.n, k[3]);
	return 0;
}

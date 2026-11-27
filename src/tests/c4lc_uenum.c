// c4lc_uenum.c: -funsigned-char and -fshort-enums (docs/c4cxx-plan.md 3.9),
// as gcc -m32 -funsigned-char -fshort-enums has them. Blood is built so:
// Watcom's char was unsigned, and its enum the smallest type that held
// the values. The golden is gcc's (c4lc-uenum.txt), from
// gcc -m32 -funsigned-char -fshort-enums -include stdio.h. c4lc takes it
// with -P -conforming (the \x escapes), and c4fc, which has no typedef,
// with -D NOTYPEDEF too. (gstr is 8 bytes, a word: c4fc gives a shorter
// global char array of a string more room than c4lc when a literal follows.)

enum small { A, B, C = 200 };
enum neg { N1 = -1, N2 = 100 };
enum mid { M = 1000 };
enum sneg { S = -1000 };
enum big { BG = 70000 };
#ifndef NOTYPEDEF
typedef enum { T0, T1 } tdef;
#else
#define tdef enum small
#endif
enum fwd;
struct s { char c; enum small e; enum neg n; short h; enum mid m; };
enum small gs = C;
enum small *pg = &gs;
enum neg gtab[3] = { N1, N2, N1 };
char gstr[8] = "\xfe\x7f";

int f (enum neg n)
{
	return n;
}

enum small g (int v)
{
	return v;
}

int main (void)
{
	char c = 200;
	signed char sc = 200;
	char buf[4] = { (char)0xf0, 1, 2, 3 };
	char *p = "\xff\x80";
	enum neg n;
	enum small e;
	enum mid m;
	enum small e2 = B;
	struct s st;
	int i;
	printf ("%d %d %d\n", (int)sizeof (enum small), (int)sizeof (enum neg), (int)sizeof (enum mid));
	printf ("%d %d %d\n", (int)sizeof (enum sneg), sizeof (enum big) == sizeof (int), (int)sizeof (tdef));
	printf ("%d %d\n", (int)sizeof (struct s), (int)sizeof gtab);
	printf ("%d %d\n", c, sc);
	printf ("%d %d\n", buf[0], c < 0);
	printf ("%d %d\n", p[0], *p);
	printf ("%d %d\n", "\x90"[0], '\xff');
	printf ("%d %d\n", gstr[0], gstr[1]);
	e = C;
	e = e + 100;
	printf ("%d %d\n", e, e2);
	n = N1;
	printf ("%d %d\n", n, f (n));
	printf ("%d %d\n", g (511), (enum small)300);
	m = 70000;
	printf ("%d\n", m);
	printf ("%d %d\n", *pg, gtab[2]);
	c = -1;
	printf ("%d %d\n", c, c == 255);
	i = c;
	printf ("%d\n", i >> 4);
	st.c = 255;
	st.e = 255;
	st.n = 255;
	st.h = -2;
	st.m = 65535;
	printf ("%d %d %d\n", st.c, st.e, st.n);
	printf ("%d %d\n", st.h, st.m);
	st.n--;
	st.e++;
	printf ("%d %d\n", st.n, st.e);
	/* multi-character constants, gcc's: each byte shifted in from the
	   right, the last four kept (Blood's demos are signed '\x1aMED') */
	i = 0x1a4d4544;
	printf ("%x %x %x %x\n", 'ab', '\x1aMED', 'abcde', '\xff\x01');
	printf ("%d %d\n", i == '\x1aMED', i != '\x1aMED');
	return 0;
}

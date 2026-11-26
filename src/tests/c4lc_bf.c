// c4lc_bf.c: bit-fields, what they hold and give back (test-c4lc, and
// test-c4fc: c4fc's images are c4lc's). Its output is gcc -m32's
// (src/c4sp/tests/expected/c4lc-bf.txt), at 32 bits and at 64, plain, -O
// and -minline: no field is as wide as a word, so nothing here depends on
// the word's size. The layouts are c4lc_bfl.c's.

struct flags {
	unsigned int a : 3;
	unsigned int b : 5;
	int s : 4;
	int t : 7;
	unsigned int big : 20;
	char c : 3;
	unsigned char uc : 5;
	short h : 9;
	unsigned short uh : 11;
};

struct pair {
	int x : 6;
	int y : 6;
};

struct mixed {
	char tag;
	unsigned int lo : 4;
	unsigned int hi : 4;
	int n;
	unsigned int z : 1;
};

union u {
	unsigned int w;
	unsigned int f : 8;
};

struct flags gf = { 5, 17, -3, 50, 999999, -2, 30, -200, 2000 };
struct pair gp[3] = { { 1, -1 }, { 31, -32 }, { -5, 7 } };
struct mixed gm = { 'q', 9, 6, -12345, 1 };

int calls;

int next(void)
{
	calls++;
	return calls;
}

struct pair *pick(struct pair *p, int i)
{
	calls = calls + 10;
	return p + i;
}

void show(struct flags *f)
{
	printf("a=%d b=%d s=%d t=%d ", f->a, f->b, f->s, f->t);
	printf("big=%d c=%d uc=%d h=%d ", f->big, f->c, f->uc, f->h);
	printf("uh=%d\n", f->uh);
}

int main(void)
{
	struct flags f;
	struct pair p;
	struct mixed m;
	union u un;
	int i, v;

	show(&gf);
	printf("gp %d %d %d %d ", gp[0].x, gp[0].y, gp[1].x, gp[1].y);
	printf("%d %d\n", gp[2].x, gp[2].y);
	printf("gm %c %d %d %d ", gm.tag, gm.lo, gm.hi, gm.n);
	printf("%d\n", gm.z);

	// stores truncate; signed fields read back with their sign
	f.a = 13; f.b = 40; f.s = 9; f.t = -70; f.big = 1048577;
	f.c = 5; f.uc = 33; f.h = 300; f.uh = 5000;
	show(&f);
	// the value of an assignment is the field's, read back
	printf("assign %d %d %d %d\n", f.a = 15, f.s = 8, f.c = -1, f.uh = 2049);
	v = f.t = 64;
	printf("chain %d %d\n", v, f.t);
	f.a = f.b = 7;
	printf("a=b %d %d\n", f.a, f.b);

	// compound assignments, ++ and --, wrapping
	f.a = 6; f.a += 3; printf("+= %d\n", f.a);
	f.s = 7; f.s += 1; printf("+= %d\n", f.s);
	f.b = 3; f.b *= 11; printf("*= %d\n", f.b);
	f.t = -64; f.t -= 1; printf("-= %d\n", f.t);
	f.big = 100; f.big <<= 4; printf("<<= %d\n", f.big);
	f.uh = 1023; f.uh |= 1024; printf("|= %d\n", f.uh);
	f.h = -255; f.h >>= 2; printf(">>= %d\n", f.h);
	f.uc = 31; printf("++ %d ", ++f.uc); printf("%d\n", f.uc);
	f.uc = 31; printf("post++ %d ", f.uc++); printf("%d\n", f.uc);
	f.s = -8; printf("-- %d ", --f.s); printf("%d\n", f.s);
	f.s = -8; printf("post-- %d ", f.s--); printf("%d\n", f.s);
	f.c = 3; f.c++; f.c++; printf("c %d\n", f.c);
	f.a = 0; f.a--; printf("a %d\n", f.a);

	// neighbours keep their bits
	f.a = 0; f.b = 31; f.a = 7; f.b = 0; f.a = 1;
	printf("neighbours %d %d\n", f.a, f.b);
	p.x = -1; p.y = 0; p.y = 21;
	printf("pair %d %d\n", p.x, p.y);

	// in expressions: promoted to int, an unsigned field included
	f.a = 2;
	printf("expr %d %d %d %d\n", f.a - 3 < 0, f.a * f.s, -f.a, f.b + f.uc);
	printf("cond %d\n", f.a > 1 ? f.a : f.b);

	// through pointers, elements, and an lvalue with side effects (once)
	calls = 0;
	pick(gp, 1)->x = 4;
	pick(gp, 2)->y += 2;
	pick(gp, 0)->x++;
	printf("pick %d %d %d %d ", gp[1].x, gp[2].y, gp[0].x, gp[0].y);
	printf("calls %d\n", calls);
	calls = 0;
	i = 0;
	gp[next()].y = 9;
	gp[next()].y -= 3;
	printf("index %d %d calls %d\n", gp[1].y, gp[2].y, calls);
	for (i = 0; i < 3; i++)
		gp[i].x = gp[i].x + i;
	printf("loop %d %d %d\n", gp[0].x, gp[1].x, gp[2].x);

	// a unit with other members around it
	m.tag = 'z'; m.lo = 15; m.hi = 1; m.n = 77; m.z = 0;
	m.hi += 9;
	m.z = 5;
	printf("mixed %c %d %d %d ", m.tag, m.lo, m.hi, m.n);
	printf("%d\n", m.z);

	// a union: the field is the word's low byte
	un.w = 0x12345678;
	printf("union %x ", un.f);
	un.f = 0xab;
	printf("%x\n", un.w);

	// whole structs copy their bits
	p = gp[2];
	printf("copy %d %d\n", p.x, p.y);
	return 0;
}

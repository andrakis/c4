// c4lc_bfl.c: bit-field layouts, at 32 bits (test-c4lc, test-c4fc). Each
// field's bits are set alone in a zeroed struct, and the struct's bytes
// printed, so the output is the layout: gcc -m32's for the default
// (src/c4sp/tests/expected/c4lc-bfl.txt), and gcc -m32 -mms-bitfields's
// with -mms-bitfields -DMS (c4lc-bfl-ms.txt), where the packed structs are
// shaped like Blood's XSPRITE and INPUT.

unsigned char *bytes;
int nbytes;

void zero(void *p, int n)
{
	int i;
	bytes = p;
	nbytes = n;
	for (i = 0; i < n; i++)
		bytes[i] = 0;
}

void dump(char *what)
{
	int i;
	printf("%s:", what);
	for (i = 0; i < nbytes; i++)
		printf(" %x", bytes[i]);
	printf("\n");
}

struct a {
	unsigned int x : 3;
	unsigned int y : 30;
	unsigned char c : 7;
	unsigned char d : 2;
	unsigned short h : 9;
	unsigned short g : 9;
	int last : 5;
};

struct b {
	char tag;
	unsigned int f : 4;
	unsigned int : 0;
	unsigned int g : 4;
	unsigned int : 5;
	unsigned int k : 3;
	short s;
	unsigned int full : 32;
};

struct c {
	unsigned char a : 4;
	unsigned int b : 8;
	unsigned char c : 4;
	unsigned short d : 12;
	unsigned char e;
};

union d {
	unsigned short h;
	unsigned char lo : 3;
	unsigned int w : 20;
};

struct e {
	unsigned int one : 1;
	struct { int n; } inner;
	unsigned int two : 2;
};

#ifdef MS
#pragma pack(push, 1)
struct xs {
	signed int reference : 15;
	unsigned int state : 1;
	unsigned int busy : 17;
	unsigned int txID : 10;
	unsigned int rxID : 10;
	unsigned int command : 8;
	unsigned int triggerOn : 1;
	unsigned int triggerOff : 1;
	unsigned int wave : 2;
	unsigned int busyTime : 12;
	unsigned int waitTime : 12;
	unsigned int restState : 1;
	unsigned int interruptable : 1;
	unsigned char respawnPending : 2;
	unsigned char key;
	signed char ofs : 4;
	unsigned short lifeLeech : 8;
};
struct in {
	unsigned char flags;
	short forward;
	short q16turn;
	short strafe;
	unsigned int run : 1;
	unsigned int jump : 1;
	unsigned int crouch : 1;
	unsigned int shoot : 1;
	unsigned int action : 1;
	unsigned int lookCenter : 1;
	unsigned int newWeapon : 8;
	unsigned char key2 : 1;
	unsigned char spin : 1;
	unsigned short look : 3;
	unsigned int last : 12;
};
#pragma pack(pop)
#ifndef NODBL
// c4fc has no double (test-c4fc defines NODBL for both compilers)
struct dbl {
	char c;
	double d;
	unsigned int f : 3;
};
#endif
#endif

int main(void)
{
	struct a a;
	struct b b;
	struct c c;
	union d d;
	struct e e;
	printf("sizes %d %d %d ", sizeof(struct a), sizeof(struct b), sizeof(struct c));
	printf("%d %d\n", sizeof(union d), sizeof(struct e));
	zero(&a, sizeof a); a.x = 7; dump("a.x");
	zero(&a, sizeof a); a.y = 0x3fffffff; dump("a.y");
	zero(&a, sizeof a); a.c = 127; dump("a.c");
	zero(&a, sizeof a); a.d = 3; dump("a.d");
	zero(&a, sizeof a); a.h = 511; dump("a.h");
	zero(&a, sizeof a); a.g = 511; dump("a.g");
	zero(&a, sizeof a); a.last = -1; dump("a.last");
	zero(&b, sizeof b); b.tag = 1; b.f = 15; dump("b.f");
	zero(&b, sizeof b); b.g = 15; dump("b.g");
	zero(&b, sizeof b); b.k = 7; dump("b.k");
	zero(&b, sizeof b); b.s = -1; dump("b.s");
	zero(&b, sizeof b); b.full = 0xffffffff; dump("b.full");
	printf("b.full %u\n", b.full);
	zero(&c, sizeof c); c.a = 15; dump("c.a");
	zero(&c, sizeof c); c.b = 255; dump("c.b");
	zero(&c, sizeof c); c.c = 15; dump("c.c");
	zero(&c, sizeof c); c.d = 0xfff; dump("c.d");
	zero(&c, sizeof c); c.e = 255; dump("c.e");
	zero(&d, sizeof d); d.lo = 7; dump("d.lo");
	zero(&d, sizeof d); d.w = 0xfffff; dump("d.w");
	zero(&e, sizeof e); e.one = 1; e.two = 3; e.inner.n = 0x01020304; dump("e");
#ifdef MS
	{
		struct xs x;
		struct in i;
		printf("ms sizes %d %d\n", sizeof(struct xs), sizeof(struct in));
		zero(&x, sizeof x); x.reference = -1; dump("xs.reference");
		zero(&x, sizeof x); x.busy = 0x1ffff; dump("xs.busy");
		zero(&x, sizeof x); x.rxID = 0x3ff; dump("xs.rxID");
		zero(&x, sizeof x); x.wave = 3; x.interruptable = 1; dump("xs.wave");
		zero(&x, sizeof x); x.respawnPending = 3; x.key = 0x55; dump("xs.respawn");
		zero(&x, sizeof x); x.ofs = -1; x.lifeLeech = 0x81; dump("xs.ofs");
		printf("xs.ofs %d %d\n", x.ofs, x.lifeLeech);
		zero(&i, sizeof i); i.run = 1; i.lookCenter = 1; i.newWeapon = 0xff; dump("in.run");
		zero(&i, sizeof i); i.key2 = 1; i.spin = 1; i.look = 7; dump("in.key2");
		zero(&i, sizeof i); i.last = 0xfff; dump("in.last");
	}
#ifndef NODBL
	{
		struct dbl q;
		printf("dbl %d\n", sizeof(struct dbl));
		zero(&q, sizeof q); q.c = 1; q.f = 7; dump("dbl");
	}
#endif
#endif
	return 0;
}

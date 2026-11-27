// c4lc_anon.c: anonymous struct and union members (C11), as chocolate-
// blood's a_asm.c has them: `union { uint16_t lw; struct { uint32_t l, h;
// }; uint64_t m; }`. A member's members are the outer struct's, at its
// offset, through `.` and `->`, nested, and in an initializer the
// anonymous member takes its own braces. The golden is gcc -m32's
// (c4lc-anon.txt, gcc -include stdio.h); c4lc only (c4fc has none).

typedef union {
	unsigned short lw;
	struct {
		unsigned int l, h;
	};
} tc_t;

typedef union {
	struct {
		unsigned char a, b, c, d;
	};
	unsigned int m;
} col_t;

struct outer {
	int x;
	union {
		int i;
		struct {
			short s1, s2;
		};
	};
	int y;
};

struct deep {
	char tag;
	struct {
		int u;
		union {
			short v;
			unsigned char w[2];
		};
	};
};

col_t gc = { { 1, 2, 3, 4 } };

int main (void)
{
	col_t c;
	struct outer o;
	struct outer *po = &o;
	struct deep d;
	tc_t t;
	c.m = 0x04030201;
	printf ("%d %d\n", c.a, c.d);
	c.b = 9;
	printf ("%d\n", c.m == 0x04030901);
	o.x = 1;
	o.i = 0;
	o.s2 = 7;
	o.y = 3;
	printf ("%d %d %d\n", o.x, o.s2, o.y);
	printf ("%d\n", po->i == 7 << 16);
	po->s1 = 5;
	printf ("%d %d\n", o.s1, po->s2);
	t.l = 0x10002;
	t.h = 5;
	printf ("%d %d\n", t.lw, t.h);
	d.tag = 'q';
	d.u = 11;
	d.v = 0x0201;
	printf ("%d %d %d\n", d.tag, d.u, d.w[1]);
	d.w[0]++;
	printf ("%d\n", d.v);
	printf ("%d %d\n", gc.b, gc.m == 0x04030201);
	return 0;
}

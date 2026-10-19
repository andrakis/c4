// c4lc_m4.c -- M4: signed, unsigned, short and long, against gcc.
//
// Two goldens, src/c4sp/tests/expected/c4lc-m4-32.txt and -64.txt, both
// gcc's: `gcc -m32` and plain `gcc` on this file. The c4 image built at
// each word size must print the same bytes. So every value printed here is
// one gcc and c4 agree on:
//
//   - int stays inside 32 bits (c4's int is a word; gcc's is 32 bits)
//   - unsigned int is 32 bits on both, at both sizes
//   - long and unsigned long are a word on both (gcc is ILP32 / LP64)
//   - nothing mixes long with unsigned int, where LP64 gcc converts to long
//     and c4 -- whose long IS int -- converts to unsigned int
//   - no sizeof of int, unsigned, long or a struct: c4 gives every scalar
//     a word and every struct member a word
//
// It is in c4fc's subset as well (no typedef, no compound assignment,
// declarations at the top of a block), because C4FC_GEN_SWEEP and
// C4SC_BACK_DIFF pin it byte for byte across c4lc, c4fc and c4sc.

unsigned int gu = 4000000000u;
unsigned char guc = 200;
signed char gsc = -100;
short gs = -1234;
unsigned short gus = 60000;
unsigned long gul = 3000000000ul;
short garr[5] = { 1, -2, 300, -32768, 32767 };
unsigned short guarr[4] = { 65535, 0, 1, 40000 };
unsigned char gbytes[4] = { 255, 128, 1, 0 };
signed char gsbytes[3] = { -1, 127, -128 };
long glong = -123456789;

struct S {
	short a;
	unsigned short b;
	unsigned char c;
	signed char d;
	unsigned int e;
	short arr[3];
	unsigned char tag[5];
};
struct S gstruct;

unsigned char ret_uc(int x) { return x; }
short ret_s(short x) { return x + 1; }
unsigned ret_u(unsigned a, unsigned b) { return a - b; }
signed char ret_sc(int x) { return x; }
unsigned short ret_us(long x) { return x; }

unsigned crc32(char *s, int n) {
	unsigned c;
	int i, k;
	c = 0xFFFFFFFFu;
	for (i = 0; i < n; i++) {
		c = c ^ (unsigned char)s[i];
		for (k = 0; k < 8; k++)
			c = (c & 1) ? (c >> 1) ^ 0xEDB88320u : c >> 1;
	}
	return c ^ 0xFFFFFFFFu;
}

unsigned fnv1a(char *s) {
	unsigned h;
	h = 2166136261u;
	while (*s) {
		h = (h ^ (unsigned char)*s) * 16777619u;
		s++;
	}
	return h;
}

void t_globals() {
	printf("globals %u %d %d %d\n", gu, guc, gsc, gs);
	printf("globals2 %d %lu %ld\n", gus, gul, glong);
	printf("garr %d %d %d %d %d\n", garr[0], garr[1], garr[2], garr[3], garr[4]);
	printf("guarr %d %d %d %d\n", guarr[0], guarr[1], guarr[2], guarr[3]);
	printf("gbytes %d %d %d %d\n", gbytes[0], gbytes[1], gbytes[2], gbytes[3]);
	printf("gsbytes %d %d %d\n", gsbytes[0], gsbytes[1], gsbytes[2]);
}

void t_wrap() {
	unsigned int u;
	unsigned char c;
	unsigned short s;
	short ss;
	signed char sc;
	u = 4294967295u; u = u + 1; printf("u+1 %u\n", u);
	u = 0; u = u - 1; printf("0-1 %u\n", u);
	u = 65536u; u = u * 65536u; printf("mul %u\n", u);
	u = 3000000000u; u = u + 3000000000u; printf("add %u\n", u);
	u = 123456789; u = u * 1000u; printf("mul2 %u\n", u);
	c = 250; c = c + 10; printf("uc %d\n", c);
	c = 0; c--; printf("uc-- %d\n", c);
	s = 65535; s++; printf("us++ %d\n", s);
	ss = 32767; ss++; printf("ss++ %d\n", ss);
	sc = 127; sc++; printf("sc++ %d\n", sc);
	u = -1; printf("u=-1 %u\n", u);
	printf("~0u %u\n", ~0u);
	printf("-5u %u\n", -5u);
	u = 7; printf("-u %u ~u %u\n", -u, ~u);
	printf("pre %u\n", ++u);
	printf("post %u", u++);
	printf(" %u\n", u);
	u = 0; printf("pre0 %u\n", --u);
}

void t_cmp() {
	unsigned int a, b;
	int i;
	unsigned long la;
	unsigned char uc;
	signed char sc;
	short sh;
	unsigned short us;
	a = 3000000000u; b = 5;
	printf("cmp %d %d %d %d %d %d\n", a > b, a < b, a >= b, a <= b, a == b, a != b);
	i = -1;
	printf("mix %d %d %d\n", i < 1u, (unsigned)i > 1u, -1 < 0u);
	la = ~0ul;
	printf("ul %d %d %d\n", la > 1ul, la == ~0ul, -1 < 1ul);
	uc = 255; sc = -1; sh = -1; us = 65535;
	printf("narrow %d %d %d %d %d\n", uc > 0, sc < 0, sh < 0, us > 0, sh == us);
	printf("eq %d %d\n", (unsigned char)-1 == 255, (unsigned)-1 == 4294967295u);
	a = 0x80000000u; b = 0x7FFFFFFFu;
	printf("sign %d %d\n", a > b, (int)(a - 1u == b));
}

void t_div() {
	unsigned int a, b;
	unsigned long la, m;
	int i;
	a = 4000000000u; b = 3;
	printf("div %u %u\n", a / b, a % b);
	a = 4294967295u;
	printf("div2 %u %u %u\n", a / 2u, a % 10u, a / 65536u);
	printf("div3 %u %u\n", a / 4000000000u, a % 4000000000u);
	la = 4000000000ul;
	printf("ldiv %lu %lu\n", la / 7ul, la % 7ul);
	m = ~0ul;
	printf("max %lu %lu %lu\n", m / 3ul, m % 1000ul, m / (m - 1ul));
	printf("max2 %lu %lu\n", (m - 5ul) % (m - 1ul), (m - 1ul) / m);
	i = -7;
	printf("sdiv %d %d %u\n", i / 2, i % 2, (unsigned)i / 2u);
	b = 7;
	printf("var %u %u\n", a / b, a % b);
}

void t_shift() {
	unsigned u;
	int i, n;
	unsigned long ul;
	unsigned char c;
	signed char sc;
	u = 0x80000000u;
	printf("shr %u %u\n", u >> 31, u >> 4);
	i = -16;
	printf("sar %d\n", i >> 2);
	ul = ~0ul;
	printf("lshr %lu %lu\n", ul >> 1, ul >> 63 % (sizeof(long) * 8));
	n = 3;
	printf("var %u %lu %u\n", u >> n, ul >> n, u >> (n - 3));
	printf("shl %u %u\n", 1u << 31, 0xFFFFFFFFu << 4);
	c = 0x80; sc = -128;
	printf("narrow %d %d\n", c >> 7, sc >> 7);
	printf("fold %u %u\n", 0xFFFFFFFFu >> 4, (0x12345678u << 8) >> 8);
}

void t_arrays() {
	short a[4];
	unsigned short ua[3];
	unsigned char b[3];
	short init[3] = { -3, 40000, 7 };
	unsigned char str[6] = "hiyo";
	short *p;
	a[0] = -1; a[1] = 70000; a[2] = -32768; a[3] = 12345;
	printf("short %d %d %d %d\n", a[0], a[1], a[2], a[3]);
	p = a;
	p++;
	printf("step %d %d\n", *p, (int)((char *)p - (char *)a));
	printf("sizeof %d %d %d %d\n", (int)sizeof(short), (int)sizeof(a), (int)sizeof(unsigned char), (int)sizeof(ua));
	ua[0] = -1; ua[1] = 65536; ua[2] = 1234;
	printf("ushort %d %d %d\n", ua[0], ua[1], ua[2]);
	b[0] = 300; b[1] = -1; b[2] = 'x';
	printf("uchar %d %d %d\n", b[0], b[1], b[2]);
	printf("init %d %d %d\n", init[0], init[1], init[2]);
	printf("str %d %d %d %d\n", str[0], str[1], str[2], str[3]);
	p = &a[3];
	*p = *p + 1;
	printf("ptr %d %d\n", a[3], p[-1]);
}

void t_casts() {
	int i;
	long l;
	i = 300;
	printf("cast %d %d %d %d\n", (char)i, (unsigned char)i, (short)70000, (unsigned short)-1);
	printf("cast2 %u %d %lu %d\n", (unsigned)-1, (signed char)200, (unsigned long)(unsigned)-1, (int)(unsigned char)-1);
	l = -2;
	printf("cast3 %u %d %d\n", (unsigned)(unsigned short)l, (short)(unsigned short)50000, (unsigned char)(l * 3));
}

void t_struct() {
	struct S s;
	struct S *p;
	s.a = -5; s.b = 65000; s.c = 255; s.d = -3; s.e = 4000000000u;
	s.arr[0] = -7; s.arr[1] = 0; s.arr[2] = 30000;
	s.tag[0] = 'a'; s.tag[1] = 200;
	p = &s;
	printf("struct %d %d %d %d\n", p->a, p->b, p->c, p->d);
	printf("struct1 %u %d %d\n", p->e, p->arr[0], p->arr[2]);
	p->b++; p->c++; p->d--; p->arr[1]--;
	printf("struct2 %d %d %d %d %d\n", p->b, p->c, p->d, p->arr[1], p->tag[1]);
	gstruct.a = 1000; gstruct.e = p->e + 300000000u;
	printf("gstruct %d %u\n", gstruct.a, gstruct.e);
}

void t_uac() {
	unsigned char a, b;
	unsigned u;
	int i;
	long l;
	unsigned long ul;
	a = 200; b = 100;
	printf("promote %d %d\n", a + b, a - b - b - b);
	u = 10; i = -20;
	printf("uac %u %d\n", u + i, u + i > 0);
	l = -20;
	printf("long %ld %ld\n", l + 10, l * 3);
	ul = 10;
	printf("ulong %lu %d\n", ul + i, ul + i > 5);
	i = 1;
	printf("cond %d %u\n", (i ? -1 : 0u) > 0, i ? -1 : 0u);
}

void t_calls() {
	printf("ret %d %d %u %d %d\n", ret_uc(300), ret_s(32767), ret_u(1, 2), ret_sc(255), ret_us(-1));
	printf("crc %u %u\n", crc32("hello", 5), crc32("The quick brown fox jumps over the lazy dog", 43));
	printf("fnv %u %u\n", fnv1a("hello"), fnv1a(""));
}

void t_printf() {
	printf("fmt %hd %hu %ld %lu %x %o\n", 70000, -1, -5l, 5ul, 0xDEADBEEFu, 8u);
	printf("fmt2 %hhx %hhd %5u|%-5u|\n", 0x1FF, 200, 42u, 7u);
}

void t_loop() {
	unsigned i, sum;
	sum = 0;
	for (i = 10; i > 0; i--) sum = sum + i;
	printf("loop %u %u\n", sum, i);
	i = 0;
	do { i = i + 0x40000000u; } while (i != 0);
	printf("wrapped %u\n", i);
}

int main() {
	t_globals();
	t_wrap();
	t_cmp();
	t_div();
	t_shift();
	t_arrays();
	t_casts();
	t_struct();
	t_uac();
	t_calls();
	t_printf();
	t_loop();
	return 0;
}

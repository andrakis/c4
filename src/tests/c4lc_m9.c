// c4lc_m9.c -- M9: what Wolfenstein 3D's port had to write around, which
// c4lc and c4fc now take as it is (docs/c4lc-design.md, "M9"). The golden
// (src/c4sp/tests/expected/c4lc-m9.txt) is gcc's, and the same at 32 and
// 64 bits: sizes are printed in units of long, which is the word to c4
// and to gcc alike, or of char.
//
//   - several function declarators in one declaration
//   - array parameters: char *argv[], int m[][4]
//   - braces around a scalar's initializer
//   - a struct passed by value
//   - sizeof of an expression in a constant: an array size, an initializer
//   - arrays of arrays: globals, locals, statics, struct members, strings
//   - C's struct layout: each member at its own alignment, padding
//   - #pragma pack(n), pack(push, n), pack(pop) and pack()

int add1(int a), twice(int b);
int add1(int a) { return a + 1; }
int twice(int b) { return b * 2; }

// ---- array parameters ----

int count(int argc, char *argv[]) {
	int i, n;
	n = 0;
	for (i = 0; i < argc; i++) n = n + argv[i][0];
	return n;
}

// ---- scalars in braces ----

int sb = { 41 };
const char *sbs = { "braced" };

// ---- a struct by value ----

struct point { long x, y; };
struct rect { struct point ul, lr; char tag; };

long width(struct rect r) { return r.lr.x - r.ul.x; }
long mix(int a, struct point p, int b, struct rect r, int c) {
	r.tag = 9;
	return a * 1000 + p.x * 100 + p.y * 10 + b + c + r.tag + width(r);
}
long bump(struct point p) { p.x = p.x + 1; return p.x; }

// ---- sizeof in constants ----

struct item { long a; char *name; short s; char tag; };
struct item menu[] = { { 1, "one", 2, 3 }, { 4, "two", 5, 6 }, { 7, "three", 8, 9 } };
struct info { long x, y, amount, cur; };
struct info items = { 10, 20, sizeof(menu) / sizeof(*(menu)), 0 };
long table[17];
int ntable = sizeof(table) / sizeof(table[0]);
char msg[] = "hello";
int nmsg = sizeof msg;

// ---- arrays of arrays ----

long grid[4][4];
char names[][8] = { "alpha", "be", "gamma" };
short small[2][3] = { { 1, 2, 3 }, { 4, 5, 6 } };
long flat[][2] = { 1, 2, 3, 4, 5, 6 };
unsigned char shade[3][256];
struct cell { long a; char tag[2][3]; } cells[2] = { { 7, { "ab", "cd" } }, { 8, { "ef", "gh" } } };
static char saves[10][16];
long cube[2][3][4];

long sum(long m[][4], int n) {
	long s;
	int i, j;
	s = 0;
	for (i = 0; i < n; i++) for (j = 0; j < 4; j++) s = s + m[i][j];
	return s;
}
long rowsum(long *r) { return r[0] + r[3]; }

// ---- layout ----

struct la { char c; long l; short s; char d; };
struct lb { char x[3]; };
struct lc { short h; char t; };
struct ld { char c; struct lc in; long w; char e[5]; };
union lu { char c; long l; short s[3]; };
struct le { struct lb bb[2]; char z; };
struct la A = { 1, 2, 3, 4 };
struct lb B[3] = { { "ab" }, { "cd" }, { "ef" } };
struct ld D = { 5, { 6, 7 }, 8, "four" };

// ---- #pragma pack ----

#pragma pack(1)
struct pa { short tag; long offs[2]; char c; };
struct pin { char c; long l; };
#pragma pack(push, 2)
struct pb { char c; long l; char d; };
#pragma pack(pop)
struct pc { char c; short s; long l; };
#pragma pack()
struct pd { char c; long l; struct pin in; char z; };
struct pd PD;
unsigned char pbytes[64];

long off(char *base, char *at) { return at - base; }

int main() {
	char *args[3];
	int i, j, k, z = { 5 };
	struct rect q;
	struct point pt;
	struct rect *rp;
	struct rect arr[2];
	long loc[3][2] = { { 1, 2 }, { 3, 4 }, { 5, 6 } };
	char lnames[2][4] = { "ab", "c" };
	char data[sizeof(table) / sizeof(*(table)) * 3];
	unsigned char *cs;
	struct le E;
	struct lc C[4];
	char *d, *t;
	struct pa *P;

	printf("decls %d %d\n", add1(1), twice(3));
	args[0] = "a"; args[1] = "b"; args[2] = "c";
	printf("argv %d\n", count(3, args));
	printf("braces %d %s %d\n", sb, sbs, z);

	q.ul.x = 1; q.ul.y = 2; q.lr.x = 11; q.lr.y = 12; q.tag = 3;
	pt.x = 4; pt.y = 5; rp = &q; arr[1] = q;
	printf("byvalue %ld %ld %ld\n", width(q), width(*rp), width(arr[1]));
	printf("byvalue %ld %d\n", mix(7, pt, 8, q, 100), q.tag);
	printf("byvalue %ld %ld %ld\n", bump(pt), pt.x, bump(q.lr));

	printf("sizeof %ld %d %d %d\n", items.amount, ntable, nmsg, (int)sizeof(data));

	for (i = 0; i < 4; i++) for (j = 0; j < 4; j++) grid[i][j] = i * 10 + j;
	for (i = 0; i < 2; i++) for (j = 0; j < 3; j++) for (k = 0; k < 4; k++) cube[i][j][k] = i * 100 + j * 10 + k;
	shade[2][255] = 9; cs = shade[2];
	d = saves[3]; t = "slot3"; while ((*d++ = *t++)) ;
	printf("rows %ld %ld %ld %ld\n", grid[3][2], sum(grid, 4), rowsum(grid[2]), *(&grid[1][1] + 1));
	printf("rows %s %s %d %d\n", names[2], names[1], (int)sizeof(names), (int)sizeof(names[0]));
	printf("rows %d %ld %d %d\n", small[1][2], flat[2][1], (int)(sizeof(flat) / sizeof(flat[0])), cs[255]);
	printf("rows %s %d %c %ld\n", saves[3], (int)sizeof(saves[3]), cells[1].tag[1][0], cells[0].a);
	printf("rows %ld %d %d %ld\n", cube[1][2][3], (int)(sizeof(cube[1]) / sizeof(long)), (int)(sizeof(cube[1][2]) / sizeof(long)), loc[2][1]);
	printf("rows %s %s %ld\n", lnames[0], lnames[1], **grid + *grid[3]);

	printf("layout %d %d %d %d %d\n", (int)(sizeof(struct la) / sizeof(long)), (int)sizeof(struct lb),
		(int)sizeof(struct lc), (int)sizeof(union lu), (int)sizeof(struct le));
	printf("layout %ld %ld %ld %ld\n", off((char *)&A, (char *)&A.l) / sizeof(long),
		off((char *)&A, (char *)&A.d) - off((char *)&A, (char *)&A.s), off((char *)&D, (char *)&D.in),
		(off((char *)&D, (char *)&D.e) - off((char *)&D, (char *)&D.w)) / sizeof(long));
	printf("layout %d %ld %d %ld %s %s\n", A.c + A.d, A.l + A.s, D.in.h + D.in.t, D.w, D.e, B[2].x);
	for (i = 0; i < 4; i++) { C[i].h = i * 100; C[i].t = i; }
	E.z = 9; E.bb[1].x[2] = 7;
	printf("layout %d %d %d %d\n", C[3].h + C[3].t, (int)((char *)&C[2] - (char *)&C[0]), E.z, E.bb[1].x[2]);

	for (i = 0; i < 64; i++) pbytes[i] = i;
	P = (struct pa *)pbytes;
	printf("pack %d %d %d %d %d\n", (int)(sizeof(struct pa) - 2 * sizeof(long)), (int)(sizeof(struct pin) - sizeof(long)),
		(int)(sizeof(struct pb) - sizeof(long)), (int)(sizeof(struct pc) - sizeof(long)), (int)(sizeof(struct pd) / sizeof(long)));
	printf("pack %d %d %d %d\n", P->tag, (int)(P->offs[0] & 0xffff), (int)(P->c - 2 * sizeof(long)),
		(int)(off((char *)&PD, (char *)&PD.z) - 3 * sizeof(long)));
	return 0;
}

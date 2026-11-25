/* Initialised local arrays and structs (c4lc-array-init): what c4lc
   copies from a template in the data and what it still stores one by
   one, either side of g:LTMPL -- strings with and without a zero tail,
   short lists, numbers, structs and pointers to strings, nested braces,
   designators, constants among expressions, and a fresh copy in every
   call of a recursion and every pass of a loop. c4fc compiles it to the
   same bytes (test-c4fc); c4lc_linitlc.c has what c4fc does not take.
   gcc wrote the golden, the same at 32 and 64 bits. */
#include <stdio.h>

struct pt { int x; char c; short s; char *name; };
struct box { char tag[6]; int v[3]; };

int g1 = 7;
int calls;

int bump(int v) { calls = calls + 1; return v * 10; }

void dumpc(char *what, char *s, int n) {
	int i;
	printf("%s:", what);
	for (i = 0; i < n; i++) printf(" %d", s[i]);
	printf("\n");
}
void dumpi(char *what, int *a, int n) {
	int i;
	printf("%s:", what);
	for (i = 0; i < n; i++) printf(" %d", a[i]);
	printf("\n");
}

void strings() {
	char a[] = "hello, world";
	char b[32] = "abc";
	char c[4] = "abcd";
	char d[16] = "";
	char e[3] = "xy";
	char f[2] = "z";
	char g[] = "tab-here";
	unsigned char u[8] = {255, 128, 97};
	printf("%s %d|%s|%d\n", a, (int)sizeof a, b, (int)sizeof b);
	dumpc("b", b, 32);
	dumpc("c", c, 4);
	dumpc("d", d, 16);
	dumpc("e", e, 3);
	dumpc("f", f, 2);
	dumpc("g", g, (int)sizeof g);
	dumpc("u", (char *)u, 8);
}

void numbers() {
	int a[10] = {1, 2, 3};
	int b[3] = {4, 5, 6};
	int z[64] = {0};
	int n[5] = {-1, 0, -2147483647, 65536, 0};
	short h[8] = {1, -2, 3, 4};
	unsigned short uh[5] = {65535, 2, 0, 4, 5};
	char sc[6] = {1, -1, 127, -128, 0, 9};
	int one[1] = {42};
	int two[2] = {7, 8};
	int seven[7] = {1, 2, 3, 4, 5, 6, 7};
	int eight[8] = {1, 2, 3, 4, 5, 6, 7, 8};
	short *sp;
	int i;
	dumpi("a", a, 10);
	dumpi("b", b, 3);
	for (i = 0; i < 64; i++) if (z[i]) printf("z[%d] %d\n", i, z[i]);
	dumpi("n", n, 5);
	printf("h %d %d %d|", h[0], h[1], h[2]);
	printf("%d %d %d\n", h[3], h[4], h[7]);
	sp = (short *)uh;
	printf("uh %d %d %d %d %d\n", sp[0], sp[1], sp[2], sp[3], sp[4]);
	dumpc("sc", sc, 6);
	printf("one %d two %d %d\n", one[0], two[0], two[1]);
	dumpi("seven", seven, 7);
	dumpi("eight", eight, 8);
}

void structs() {
	struct pt p = {1, 'a', -3, "one"};
	struct pt ps[3] = {{1, 'a', 2, "one"}, {3, 'b', 4, "two"}, {5, 'c', 6, "three"}};
	struct pt q[4] = {{9, 'q', 1, "q"}};
	struct box bx = {"tag", {1, 2, 3}};
	struct box bs[2] = {{"ab", {4}}, {"cdefg", {5, 6, 7}}};
	char *names[] = {"zero", "one", "two", "three", "four"};
	char *few[6] = {"a", 0, "c"};
	int i;
	printf("p %d %c %d %s\n", p.x, p.c, p.s, p.name);
	for (i = 0; i < 3; i++) printf("ps %d %c %d %s\n", ps[i].x, ps[i].c, ps[i].s, ps[i].name);
	for (i = 0; i < 4; i++) printf("q %d %d %d %d\n", q[i].x, q[i].c, q[i].s, q[i].name != 0);
	printf("bx %s %d %d %d\n", bx.tag, bx.v[0], bx.v[1], bx.v[2]);
	for (i = 0; i < 2; i++) printf("bs %s %d %d %d %d\n", bs[i].tag, bs[i].v[0], bs[i].v[1], bs[i].v[2], bs[i].tag[5]);
	for (i = 0; i < 5; i++) printf("%s ", names[i]);
	printf("\n");
	for (i = 0; i < 6; i++) printf("%s ", few[i] ? few[i] : "-");
	printf("\n");
}

void nested() {
	int m[3][4] = {{1, 2}, {3, 4, 5}, {6}};
	int flat[2][3] = {1, 2, 3, 4};
	char words[3][8] = {"red", "green", "blue"};
	struct box deep[2][2] = {{{"a", {1}}, {"b", {2, 3}}}, {{"c", {4, 5, 6}}}};
	int i, j;
	for (i = 0; i < 3; i++) { for (j = 0; j < 4; j++) printf(" %d", m[i][j]); printf(";"); }
	printf("\n");
	for (i = 0; i < 2; i++) for (j = 0; j < 3; j++) printf(" %d", flat[i][j]);
	printf("\n");
	for (i = 0; i < 3; i++) printf("%s %d|", words[i], words[i][7]);
	printf("\n");
	for (i = 0; i < 2; i++) for (j = 0; j < 2; j++)
		printf("%s %d %d %d|", deep[i][j].tag, deep[i][j].v[0], deep[i][j].v[1], deep[i][j].v[2]);
	printf("\n");
}

void mixed(int x) {
	int a[8] = {1, x, 3, bump(4), 5, 6, x + 1, 8};
	int b[6] = {x, x, x};
	struct pt p[2] = {{x, 'm', 7, "mix"}, {1, 2, 3, 0}};
	int *ptrs[4] = {&g1, 0, &x, &g1};
	int d1[6] = {[1] = 2, [3] = 4, [4] = 5, [5] = x};
	int d2[6] = {[5] = 1, [2] = 3, 9, 8, 7};
	int d3[5] = {1, 2, 3, 4, [0] = x};
	int sparse[64] = {1, 2, 3, 4, [60] = 5};
	dumpi("a", a, 8);
	dumpi("b", b, 6);
	printf("p %d %c %d %s|", p[0].x, p[0].c, p[0].s, p[0].name);
	printf("%d %d %d\n", p[1].x, p[1].c, p[1].s);
	printf("ptrs %d %d %d %d\n", *ptrs[0], ptrs[1] == 0, *ptrs[2], ptrs[3] == ptrs[0]);
	dumpi("d1", d1, 6);
	dumpi("d2", d2, 6);
	dumpi("d3", d3, 5);
	printf("sparse %d %d %d %d|", sparse[0], sparse[3], sparse[4], sparse[59]);
	printf("%d %d\n", sparse[60], sparse[63]);
	printf("calls %d\n", calls);
}

/* every call has its own copy, made afresh, however deep */
int rec(int depth) {
	char s[12] = "level";
	int v[6] = {1, 2, 3, 4, 5, 6};
	int t, i;
	s[5] = '0' + depth;
	v[depth % 6] = 100 + depth;
	t = depth > 0 ? rec(depth - 1) : 0;
	for (i = 0; i < 6; i++) t = t + v[i];
	printf("%s %d %d %d\n", s, s[6], v[depth % 6], t);
	return t;
}

void loops() {
	int k;
	for (k = 0; k < 3; k++) {
		char buf[10] = "loop";
		int acc[5] = {10, 20, 30};
		buf[k] = 'X';
		acc[k + 2] = k;
		printf("%s %d %d|", buf, acc[0], acc[1]);
		printf("%d %d %d\n", acc[2], acc[3], acc[4]);
	}
}

int main() {
	strings();
	numbers();
	structs();
	nested();
	mixed(11);
	rec(7);
	loops();
	return 0;
}

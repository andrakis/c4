/* Initialised locals (c4lc-array-init), c4lc's alone: a template's
   constants beside what is still stored after the copy -- long longs,
   floats and doubles, struct values, compound literals -- and typedefs,
   _Bool, enums and unsigned elements, which c4fc does not take.
   gcc wrote the golden, the same at 32 and 64 bits. */
#include <stdio.h>

typedef int vec4[4];
typedef struct { int a; long long b; int c; int d; int e; } mixl;
typedef struct { float x; int a; int b; int c; double y; int d; } mixf;
struct pt { int x; char c; short s; char *name; };
struct pair { int *v; int n; int m; int k; };
enum { A = 5, B };

int twice(int v) { return v * 2; }
/* a long long as two ints: 32-bit c4m's printf has no %lld */
void pll(long long v) { printf(" %d:%d", (int)(v / 1000000), (int)(v % 1000000)); }

void wide() {
	long long ll[4] = {1, -2, 3000000000LL};
	mixl m = {1, 2, 3, 4, 5};
	mixl ms[2] = {{1, -1, 3, 4, 5}, {6, 7000000000LL, 8, 9, 10}};
	float f[4] = {1.5, 2.5};
	mixf mf = {1.5, 1, 2, 3, 2.25, 4};
	double d[3] = {0.5, -1.25, 3};
	int i;
	printf("ll");
	for (i = 0; i < 4; i++) pll(ll[i]);
	printf("\nm %d", m.a);
	pll(m.b);
	printf(" %d %d %d\n", m.c, m.d, m.e);
	for (i = 0; i < 2; i++) {
		printf("ms %d", ms[i].a);
		pll(ms[i].b);
		printf(" %d %d %d\n", ms[i].c, ms[i].d, ms[i].e);
	}
	printf("f %d %d %d %d\n", (int)(f[0] * 2), (int)(f[1] * 2), (int)f[2], (int)f[3]);
	printf("mf %d %d %d %d|", (int)(mf.x * 2), mf.a, mf.b, mf.c);
	printf("%d %d\n", (int)(mf.y * 4), mf.d);
	printf("d %d %d %d\n", (int)(d[0] * 4), (int)(d[1] * 4), (int)d[2]);
}

void values(int x) {
	struct pt p0 = {9, 'p', 8, "p0"};
	struct pt arr[3] = {p0, {1, 'a', 2, "one"}, p0};
	struct pair pr[2] = {{(int[]){7, 8, 9, 10}, 4, 5, 6}, {0, 1, 2, 3}};
	int k[6] = {1, 2, twice(x), 4, (int[]){11, 12, 13, 14, 15}[2], 6};
	int i;
	arr[2].x = 99;
	for (i = 0; i < 3; i++) printf("arr %d %c %d %s\n", arr[i].x, arr[i].c, arr[i].s, arr[i].name);
	printf("p0 %d\n", p0.x);
	printf("pr %d %d %d %d|", pr[0].v[0], pr[0].v[3], pr[0].n, pr[0].m);
	printf("%d %d %d %d\n", pr[0].k, pr[1].v == 0, pr[1].n, pr[1].k);
	for (i = 0; i < 6; i++) printf(" %d", k[i]);
	printf("\n");
}

void kinds() {
	vec4 v = {1, 2, 3, 4};
	_Bool b[6] = {1, 0, 1, 1, 0, 1};
	int e[4] = {A, B, A + B};
	unsigned char u[8] = {255, 128, 97};
	unsigned short uh[5] = {65535, 2, 0, 4, 5};
	unsigned int ui[4] = {4000000000u, 1, 2};
	static int st[4] = {1, 2, 3, 4};
	int i;
	st[0] = st[0] + 10;
	printf("v %d %d %d %d\n", v[0], v[1], v[2], v[3]);
	for (i = 0; i < 6; i++) printf(" %d", (int)b[i]);
	printf("\n");
	printf("e %d %d %d %d\n", e[0], e[1], e[2], e[3]);
	printf("u %d %d %d %d\n", (int)u[0], (int)u[1], (int)u[2], (int)u[7]);
	printf("uh %d %d %d %d %d\n", (int)uh[0], (int)uh[1], (int)uh[2], (int)uh[3], (int)uh[4]);
	printf("ui %u %u %u %u\n", ui[0], ui[1], ui[2], ui[3]);
	printf("st %d\n", st[0]);
}

int main() {
	wide();
	values(21);
	kinds();
	kinds();
	return 0;
}

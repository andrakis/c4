// c4lc_m10lc.c -- M10, the part only c4lc takes (c4fc has no typedef, no
// float and no initializer computed at start-up): steps 3 and 5 of
// docs/ports-feasibility.md §8, and step 4's folding where it reaches
// further than c4fc's. The golden (src/c4sp/tests/expected/c4lc-m10lc.txt)
// is gcc's, the same at 32 and 64 bits.
//
//   - `T *p = &a[k];` for a global, set at start-up as `a + k` is
//   - floating point folded in a brace list and converted to the member's
//     type: am_map.c's `{ -.867*R, -.5*R }` for fixed_t members
//   - a double constant initializing a float, a float a double, and a
//     double an int: doomgeneric's `float mouse_acceleration = 2.0;`
//   - an address plus an offset: a row plus k, an array member, a string
//     plus k

typedef int fixed_t;
typedef int boolean;

// ---- &a[k] for a global (g_game.c, r_main.c) ----

boolean mousearray[4];
boolean *mousebuttons = &mousearray[1];
struct pt { long x, y; } pts[3];
long *py = &pts[2].y;
char grid[3][5];
char *g21 = &grid[2][1];

// ---- floating point in a brace list (am_map.c) ----

typedef struct { fixed_t x, y; } mpoint_t;
typedef struct { mpoint_t a, b; } mline_t;
// R is ((8*PLAYERRADIUS)/7) and PLAYERRADIUS 16*FRACUNIT, as gcc -E leaves them
mline_t triangle_guy[] = {
	{ { -.867*((8*(16*(1<<16)))/7), -.5*((8*(16*(1<<16)))/7) },
	  { .867*((8*(16*(1<<16)))/7), -.5*((8*(16*(1<<16)))/7) } },
	{ { .867*((8*(16*(1<<16)))/7), -.5*((8*(16*(1<<16)))/7) }, { 0, ((8*(16*(1<<16)))/7) } },
	{ { 0, ((8*(16*(1<<16)))/7) }, { -.867*((8*(16*(1<<16)))/7), -.5*((8*(16*(1<<16)))/7) } }
};
float fl[] = { 2.0, 1 / 3.0, (float)(1.0 / 3), 0.1f + 0.2f, -0.0, 3, 1.5f * 2 - 1 };
double db[] = { 1.5f, 0.1 + 0.2, 1e-310 * 2, (double)(float)0.1, 7 / 2, 1.0 / 7, -(2.5 - 0.5) };
long cmp[] = { 1.5 < 2, 2.0 == 2, 0.5f + 0.25 >= 0.75, (long)-2.75, (unsigned char)200.9 };
struct mix { char c; short s; unsigned int u; float f; double d; } mixes[] = {
	{ 65.9, -2.5 * 3, 4e9, 1.0 / 3, 1.0f / 3 },
	{ 'z', 1.5e4, 0.5, 6, -1e-3 },
};

// ---- one float constant where another type is wanted ----

float mouse_acceleration = 2.0;
double half = 2.5f;
int truncated = 2.75;
int fromneg = -2.75;
fixed_t scale_mtof = .2*(1<<16);

// ---- an address plus an offset, further than c4fc's ----

static unsigned char seq[3][4] = { { 1, 2 }, { 3, 4 }, { 5, 6 } };
struct holder { int pad; long arr[3]; } hold = { 1, { 10, 20, 30 } };
struct cheat { unsigned char *s; unsigned char *p; char *msg; long *l; };
struct cheat cheats[] = {
	{ seq[1] + 1, seq[2], "abc" + 1, hold.arr },
	{ (unsigned char *)&seq[0][1], 0, (char *)"xyz", &hold.arr[1] },
	{ seq[2], &seq[2][1] + 1, "uvw", hold.arr + 2 },
};

// the bits of a float or a double, the same at both sizes (c4's unsigned
// int is the word, its low 32 bits)
unsigned int fbits(float *f) { return *(unsigned int *)f; }
void dbits(char *what, double *d) {
	printf("%s %08x %08x\n", what, *(unsigned int *)((char *)d + 4), *(unsigned int *)d);
}

int main() {
	int i;
	mousebuttons[-1] = 5;
	mousebuttons[2] = 7;
	*py = 9;
	*g21 = 3;
	printf("addr %d %d %ld %d\n", mousearray[0], mousearray[3], pts[2].y, grid[2][1]);
	for (i = 0; i < 3; i++)
		printf("tri %d %d %d %d\n", triangle_guy[i].a.x, triangle_guy[i].a.y, triangle_guy[i].b.x, triangle_guy[i].b.y);
	for (i = 0; i < 7; i++) printf("fl %08x\n", fbits(&fl[i]));
	for (i = 0; i < 7; i++) dbits("db", &db[i]);
	printf("cmp %ld %ld %ld %ld %ld\n", cmp[0], cmp[1], cmp[2], cmp[3], cmp[4]);
	for (i = 0; i < 2; i++) {
		printf("mix %d %d %u %08x\n", mixes[i].c, mixes[i].s, mixes[i].u, fbits(&mixes[i].f));
		dbits("mix", &mixes[i].d);
	}
	printf("one %08x %d %d %d\n", fbits(&mouse_acceleration), truncated, fromneg, scale_mtof);
	dbits("one", &half);
	for (i = 0; i < 3; i++)
		printf("cheat %d %d %s %ld\n", cheats[i].s[0], cheats[i].p ? *cheats[i].p : -1, cheats[i].msg, *cheats[i].l);
	return 0;
}

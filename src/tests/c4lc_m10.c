// c4lc_m10.c -- M10: what Doom (linuxdoom-1.10) needs, steps 1, 2 and 4 of
// docs/ports-feasibility.md §8, as far as c4fc's grammar reaches: c4lc and
// c4fc both take all of it. The golden (src/c4sp/tests/expected/c4lc-m10.txt)
// is gcc's (-std=gnu89, for the implicit int), the same at 32 and 64 bits.
//
//   - register, and `register x = 0;` with no type, which is an int
//   - extern declarations and prototypes inside a block
//   - a switch whose cases are 4096 or more apart: compares, not a table
//   - casts in constant expressions: case labels, enum values, array sizes
//   - casts and an address plus an offset in a global's brace list:
//     (int *)&x, (long)"str", &a[k], a row a[k], &a[k][j], and a table
//     naming its own element (sounds.c's S_sfx)

// ---- register ----

int clip(int a, int b) {
	register outcode1 = 0;
	register outcode2 = 0, outside;
	register int x;
	x = a + b;
	if (x < 0) outcode1 = 1;
	if (x > 100) outcode2 = 2;
	outside = outcode1 | outcode2;
	return outside * 1000 + x;
}

// ---- declarations inside a block ----

int total(void) {
	extern int forwardmove[2];
	int twice(int), *pick(int i);
	int s;
	s = twice(forwardmove[0]) + twice(forwardmove[1]);
	{
		extern int sidemove;
		void bump(int *p);
		bump(&s);
		s = s + sidemove;
	}
	return s + *pick(1);
}
int forwardmove[2] = { 0x19, 0x32 };
int sidemove = 1000;
int twice(int v) { return v * 2; }
int picks[3] = { 7, 8, 9 };
int *pick(int i) { return &picks[i]; }
void bump(int *p) { *p = *p + 100000; }

// ---- a sparse switch ----

// st_stuff.c's AM_MSGENTERED is ('a'<<24)+('m'<<16) | ('e'<<8)
int msgkind(int c) {
	switch (c) {
	case ('a' << 24) + ('m' << 16) + ('e' << 8): return 1;
	case ('a' << 24) + ('m' << 16) + ('x' << 8): return 2;
	case 4: return 3;
	case -100000: c = c + 1;
	case 0x7fffffff: return 4;
	case -0x7fffffff - 1: return 5;
	default: return 6;
	}
}
int nodefault(int c) {
	int r;
	r = 10;
	switch (c) {
	case 0x10000: r = 11; break;
	case 3: r = 12;
	}
	return r;
}

// ---- casts in constant expressions ----

// OpenTyrian's `case (Uint32)SDL_SCANCODE_Q | (Uint32)KMOD_SHIFT << 16:`
enum { KEY_Q = 20, KMOD_SHIFT = 1 };
int keyed(int k, int shift) {
	switch (k | (shift << 16)) {
	case (unsigned int)KEY_Q | (unsigned int)KMOD_SHIFT << 16: return 1;
	case (int)KEY_Q: return 2;
	case (unsigned char)300: return 3;
	}
	return 0;
}
enum { BIG = (int)(unsigned char)0x1ff, NEG = (signed char)0xf0 };
int sized[(int)sizeof(long) / (int)sizeof(long) + (unsigned char)257];

// ---- casts and addresses in a global's brace list ----

// m_misc.c's defaults[]: strings kept in integers
char *sndserver_filename = "sndserver";
int mouse = 3;
struct defaultv { char *name; int *location; long defaultvalue; };
struct defaultv defaults[] = {
	{ "mouse", &mouse, 3 },
	{ "sndserver", (int *)&sndserver_filename, (long)"sndserver" },
	{ "big", (int *)0, (char)300 },
	{ "neg", (int *)&mouse, (unsigned char)-1 },
};

// sounds.c's S_sfx[]: an element naming another of the same table
struct sfxinfo { char *name; int priority; struct sfxinfo *link; int pitch; };
extern struct sfxinfo S_sfx[];
struct sfxinfo S_sfx[] = {
	{ "none", 0, 0, -1 },
	{ "pistol", 64, 0, -1 },
	{ "chgun", 64, &S_sfx[1], 150 },
	{ "shotgn", 64, &S_sfx[1], -1 },
};

// st_stuff.c's cheats: a row of an array of arrays, and an element in one
static unsigned char cheat_seq[3][6] = { { 'i', 'd', 0xff }, { 'k', 'f', 'a', 0xff }, { 'x', 0xff } };
struct cheat { unsigned char *sequence; unsigned char *p; };
struct cheat cheats[3] = { { cheat_seq[0], 0 }, { cheat_seq[1], &cheat_seq[1][2] }, { cheat_seq[2], 0 } };

int main() {
	int i;
	printf("register %d %d %d\n", clip(1, 2), clip(-5, 1), clip(99, 9));
	printf("block %d\n", total());
	printf("sparse %d %d %d\n",
		msgkind(('a' << 24) + ('m' << 16) + ('e' << 8)), msgkind(('a' << 24) + ('m' << 16) + ('x' << 8)), msgkind(4));
	printf("sparse %d %d %d %d\n", msgkind(-100000), msgkind(0x7fffffff), msgkind(-0x7fffffff - 1), msgkind(5));
	printf("sparse %d %d %d\n", nodefault(0x10000), nodefault(3), nodefault(4));
	printf("casts %d %d %d %d\n", keyed(KEY_Q, 1), keyed(KEY_Q, 0), keyed(44, 0), keyed(5, 0));
	printf("casts %d %d\n", BIG, NEG);
	printf("casts %ld\n", (long)(sizeof(sized) / sizeof(sized[0])));
	printf("defaults %s %d %ld\n", defaults[0].name, *defaults[0].location, defaults[0].defaultvalue);
	printf("defaults %s %s %s\n", defaults[1].name, *(char **)defaults[1].location, (char *)defaults[1].defaultvalue);
	printf("defaults %s %d %ld\n", defaults[2].name, defaults[2].location == 0, defaults[2].defaultvalue);
	printf("defaults %s %d %ld\n", defaults[3].name, *defaults[3].location, defaults[3].defaultvalue);
	for (i = 0; i < 4; i++)
		printf("sfx %s %d %s %d\n", S_sfx[i].name, S_sfx[i].priority,
			S_sfx[i].link ? S_sfx[i].link->name : "-", S_sfx[i].link ? (int)(S_sfx[i].link - S_sfx) : -1);
	for (i = 0; i < 3; i++)
		printf("cheat %c %d %d\n", cheats[i].sequence[0], cheats[i].sequence[1],
			cheats[i].p ? (int)(cheats[i].p - cheats[i].sequence) * 1000 + *cheats[i].p : -1);
	return 0;
}

// c4lc_tdlist.c -- typedefs as Shadow Warrior's headers write them
// (games/sw):
//
//   typedef union { LONG l; struct { WORD lsw; WORD msw; } w; } DD, * DDp, * fDDp;
//   typedef int ANIMATOR(SHORT SpriteNum);
//   typedef ANIMATOR *ANIMATORp;
//   ANIMATOR DoBunnyMove, NullBunny, DoActorDebris, DoBunnyGrowUp;
//
// Several names in one typedef, each the base type with its own stars
// (and size), as in any declaration; and a typedef of a function type,
// whose name then declares functions, and with a * points to one.
// Exits 42.
typedef int LONG, *LONGp, **LONGpp;
typedef union { int l; struct { short lsw; short msw; } w; } DD, *DDp, *fDDp;
typedef struct { int a; int b; } PAIR, *PAIRp, PAIRS[3];

typedef int ANIMATOR(short n);
typedef ANIMATOR *ANIMATORp;
typedef ANIMATOR ACTOR;			// another name for the function type
typedef void STEP(int *), *STEPp;	// a function type and its pointer at once

ANIMATOR Twice, Thrice;			// prototypes
ACTOR Four;
STEP Bump;

ANIMATORp table[3] = { Twice, Thrice, Four };

int Twice(short n) { return 2 * n; }
int Thrice(short n) { return 3 * n; }
int Four(short n) { return 4 * n; }
void Bump(int *p) { *p = *p + 1; }

struct state { ANIMATORp go; ANIMATOR *also; };
struct state states[1] = { { Thrice, Twice } };

int main()
{
	LONG x;
	LONGp p;
	LONGpp pp;
	DD d;
	DDp dp;
	fDDp fp;
	PAIRS ps;
	PAIRp q;
	ANIMATOR Late;			// a prototype among the locals
	ANIMATOR *f;
	STEPp s;
	int k;
	x = 5;
	p = &x;
	pp = &p;
	**pp = 7;
	d.l = 0;
	d.w.lsw = 30;
	dp = &d;
	fp = dp;
	ps[2].a = 2;
	q = &ps[2];
	f = Thrice;
	s = Bump;
	k = 0;
	s(&k);					// 1
	k = k + table[1](4) - Twice(6);		// 12 - 12
	k = k + f(2) - 6 + table[2](1) - 4;	// 6 - 6 + 4 - 4
	k = k + states[0].go(2) + states[0].also(1) - Late(4);	// 6 + 2 - 8
	return x + fp->w.lsw + q->a + (sizeof (PAIRS) == 3 * sizeof (PAIR)) * 2 + k;
}

int Late(short n) { return 2 * n; }

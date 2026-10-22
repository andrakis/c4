// c4lc_m9lc.c -- M9, the part only c4lc takes (c4fc has no typedef and no
// float): a typedef inside a function, and globals whose initializers c4lc
// cannot fold -- floating point, and an address plus an offset -- which a
// constructor c4lc writes sets before main. Also an extern array no unit
// defines and nothing uses (Wolf3D's CASSERT). The golden
// (src/c4sp/tests/expected/c4lc-m9lc.txt) is gcc's, the same at 32 and
// 64 bits.

typedef int T;

int inner() {
	typedef struct { char c; long v; } T;
	T t;
	t.v = 5;
	t.c = 2;
	return t.v + t.c;
}

long sintable[360 + 90];
long *costable = sintable + (360 / 4);
const float radtoint = (float)(3600 / 2 / 3.141592657);
double half = 1.0 / 2;
int ip = (int)(1.5 * 10);
int folded = sizeof(sintable) / sizeof(sintable[0]);
extern int assert_compile[(sizeof(long) >= 4) * 2 - 1];

int main() {
	T x;
	static float sf = 2.5f * 2;
	x = 3;
	{
		typedef char U;
		U u = 65;
		printf("typedef %d %d %c\n", inner(), x, u);
	}
	sintable[90 + 3] = 77;
	printf("init %ld %d %d\n", costable[3], (int)(radtoint * 1000), (int)(half * 100));
	printf("init %d %d %d %d\n", ip, (int)sf, (int)(costable - sintable), folded);
	return 0;
}

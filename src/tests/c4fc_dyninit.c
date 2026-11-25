// c4fc_dyninit.c -- scalar globals that __c4lc_init sets at start-up
// (c4lc-parse's p:dyninit?, c4fc's DYN-FUNC): c4fc must make c4lc's
// object of this, byte for byte, and both must print
// "dyninit 30 7 9 5 4 12 1 3 8 10 2 11 60 22 7 40"
//
// sizeof, an enum constant and a static const are still folded, so
// `width` and `half` are data, not stores.
enum { ROWS = 3, COLS = 4 };
static const int base = 5;

int table[8] = { 10, 20, 30, 40, 50, 60, 70, 80 };
int grid[ROWS][COLS];
struct pt { int x; int y; } origin = { 7, 9 };
char text[] = "abcdefgh";

int *third = &table[2];                       // &a[k]
int *corner = &grid[2][3];                    // &a[i][j]
int *py = &origin.y;                          // &s.m
int *after = table + 5;                       // a + k
char *mid = (char *)&text[4];                 // a cast in front
const int *const ro = &table[1];              // a const global, set once
static int *hidden = &table[6] - 1;           // a static
int one = 1, *two = &table[1] + 1, three = 3; // among others on a line
int width = sizeof(struct pt) / sizeof(int);  // folded: data
int half = COLS / 2 + base;                   // folded: data

int twice(int v) { return v + v; }
int (*op)(int) = (int (*)(int))twice;         // a function pointer, cast

int ctor_saw;
// a constructor of the unit's own runs as well; __c4lc_init comes after it
static void __attribute__((constructor)) early(void) { ctor_saw = 1; }

int *counted() {
	static int *last = &table[7];             // a static local
	static int ring[4] = { 1, 2, 3, 4 };
	static int *r2 = &ring[2];                // ... naming another
	return *r2 == 3 ? last : 0;
}

int main() {
	grid[2][3] = 12;
	printf("dyninit %d %d %d %d", *third, origin.x, *py, *after / 12);
	printf(" %d %d %d %d", mid[0] - 'a', *corner, one, three);
	printf(" %d %d %d %d", width * 4, half + 3, (int)(two - table), op(5) + 1);
	printf(" %d %d %d %d\n", *hidden, *ro + 2, ctor_saw + 6, *counted() / 2);
	return 0;
}

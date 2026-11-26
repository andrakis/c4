// c4lc_rowptr.c: a pointer to an array, `T (*p)[N]`, declared and cast to
// (Blood's screen.cpp: static byte (*gammaTable)[256], set from a resource
// by `(unsigned char (*)[256])`), and an extern object of a struct the
// unit never defines (view.cpp's `extern AISTATE handIdle;`), which -c
// once refused. test-c4lc against gcc -m32's answers, at both sizes,
// plain, -O and -minline, and with -c; test-c4fc holds c4fc's images to
// c4lc's.

static unsigned char (*gammaTable)[4];
static int (*grid)[3][2];
unsigned char tables[3][4];
int cells[2][3][2];

struct AISTATE;
extern struct AISTATE handIdle;

struct row { int a; char b; };
struct row rows[2][3];

int sum(unsigned char (*t)[4], int n)
{
	int i, j, s;
	s = 0;
	for (i = 0; i < n; i++)
		for (j = 0; j < 4; j++)
			s = s + t[i][j] * (i + 1);
	return s;
}

int main(void)
{
	int i, j, k;
	char *raw;
	struct row (*rp)[3];
	for (i = 0; i < 3; i++)
		for (j = 0; j < 4; j++)
			tables[i][j] = i * 10 + j;
	raw = (char *)tables;
	gammaTable = (unsigned char (*)[4])raw;
	printf("gamma %d %d %d\n", gammaTable[1][2], gammaTable[2][3], sum(gammaTable, 3));
	gammaTable++;
	printf("next %d %d %d\n", gammaTable[0][0], (*gammaTable)[1], (int)sizeof(*gammaTable));
	grid = cells;
	for (i = 0; i < 2; i++)
		for (j = 0; j < 3; j++)
			for (k = 0; k < 2; k++)
				grid[i][j][k] = i * 100 + j * 10 + k;
	printf("grid %d %d\n", cells[1][2][1], grid[0][1][0]);
	rp = rows;
	rp[1][2].a = 7;
	rp[1][2].b = 'x';
	printf("rows %d %c\n", rows[1][2].a, rows[1][2].b);
	return 0;
}

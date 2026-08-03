// C4 Test: initialized globals and arrays (global and local).
// Compiled by gcc as the oracle: output must match exactly.
// The fp() macro gives gcc a typed call through the int* function
// pointer; c4cc sees the raw fp() call (it skips # lines).
enum { RED = 3, BLUE = 44 };

int x = 5;
int y = -7;
int z = BLUE;
char ch = 'A';
char *msg = "hello, globals";
int arr[5] = {10, -20, RED, 40};
char bytes[8] = {1, 2, 250};
char text[] = "abc";
int big[100];

int fortytwo () { return 42; }
int *fp = &fortytwo;
#ifndef __c4cc__
#define fp() ((int (*)())fp)()
#define lfp() ((int (*)())lfp)()
#endif

// Local initializers re-run on every entry. Two same-depth calls reuse
// the same stack memory, so if the stores did not re-run, the second
// call would see the first call's mutations instead of fresh values.
int reinit () {
	int x = 3;
	int a[2] = {10, 20};
	char t[6] = "ab";
	x = x + a[0] + a[1] + t[0] + t[4];
	a[0] = 999; a[1] = 888; t[0] = 'Z'; t[4] = 'Q';
	return x;
}

int main () {
	int i, sum;
	int larr[10];
	char lbytes[10];
	int li = 7;
	int larr2[4] = {5, -6, RED};
	int lun[] = {2, 4, 8};
	char lstr[8] = "hey";
	char *lp = "local ptr";
	int *lfp = &fortytwo;

	printf("scalars: %d %d %d %c\n", x, y, z, ch);
	printf("msg: %s\n", msg);
	sum = 0; i = 0;
	while (i < 5) { sum = sum + arr[i]; i = i + 1; }
	printf("arr sum: %d (arr[2]=%d arr[4]=%d)\n", sum, arr[2], arr[4]);
	printf("bytes: %d %d %d %d\n", bytes[0], bytes[1], bytes[2], bytes[7]);
	printf("text: %s (%d)\n", text, text[1]);
	big[42] = 4242; big[99] = 99;
	printf("big: %d %d %d\n", big[0], big[42], big[99]);
	i = 0;
	while (i < 10) { larr[i] = i * i; i = i + 1; }
	printf("larr: %d %d %d\n", larr[0], larr[5], larr[9]);
	i = 0;
	while (i < 9) { lbytes[i] = 'a' + i; i = i + 1; }
	lbytes[9] = 0;
	printf("lbytes: %s\n", lbytes);
	printf("fp: %d\n", fp());
	x = x + arr[3];
	printf("x now: %d\n", x);
	// local initializers (the VM's printf takes few arguments: keep short)
	printf("li: %d larr2: %d %d %d %d\n", li, larr2[0], larr2[1], larr2[2], larr2[3]);
	printf("lun: %d %d %d\n", lun[0], lun[1], lun[2]);
	printf("lstr: %s (%d %d)\n", lstr, lstr[3], lstr[7]);
	printf("lp: %s\n", lp);
	printf("lfp: %d\n", lfp());
	printf("reinit: %d %d\n", reinit(), reinit());
	// sizeof: int arrays compared as element counts (gcc's int is a
	// different size), char arrays as raw bytes -- identical either way
	printf("counts: %d %d %d\n",
	       sizeof(arr) / sizeof(int), sizeof(larr2) / sizeof(int),
	       sizeof(lun) / sizeof(int));
	printf("bytes-sizes: %d %d %d\n", sizeof(bytes), sizeof(text), sizeof(lstr));
	// &array is the array's own address
	printf("addr eq: %d %d\n", (int *)&arr == (int *)arr, (int *)&larr2 == (int *)larr2);
	return 0;
}

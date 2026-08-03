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
#endif

int main () {
	int i, sum;
	int larr[10];
	char lbytes[10];

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
	return 0;
}

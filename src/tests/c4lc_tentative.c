// C4 Test: a global defined more than once in one unit is one object,
// in c4lc and c4fc alike (test-c4fc compares them byte for byte; test-c4lc
// counts the symbols of its object)
// Expected output: "tentative 2 3 4 9 2"

int x;
int x;                  // the same x
int y = 3;
int y;                  // the same y, still 3
int z;
int z = 4;              // z is this one
int arr[3];
int arr[3];
int *px = &x;
int *pa = &arr[1];      // set at start-up, by __c4lc_init
int *pa;                // the same pa
int *pb;
int *pb = &arr[2];      // pb is this one

int main () {
	x = 2;
	arr[2] = 9;
	printf("tentative %d %d %d %d %d\n", x, y, z, arr[2] + (pb - pa) - 1, *px);
	return 0;
}

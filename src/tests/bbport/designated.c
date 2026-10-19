struct pt { int x; int y; int z; };
struct pt p = { .y = 2, .z = 3 };
int arr[5] = { [1] = 7, [4] = 9 };
int main() { printf("des %d %d %d %d %d\n", p.x, p.y, p.z, arr[1], arr[4]); return 0; }

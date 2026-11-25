#define Y 0
#if !Y
#define R 1
#else
#define R 3
#endif
int main() { printf("in %d\n", R); return 0; }

#define X 1
#define Y 1
#if X && Y
#define R 1
#else
#define R 3
#endif
int main() { printf("ia %d\n", R); return 0; }

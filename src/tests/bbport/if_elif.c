#define Y 1
#if 0
#define R 3
#elif Y
#define R 1
#else
#define R 2
#endif
int main() { printf("ie %d\n", R); return 0; }

#define ENABLE_X 1
#define ENABLE_Y 0
#if defined(ENABLE_X) && ENABLE_X && !ENABLE_Y
#define R 1
#elif ENABLE_Y
#define R 2
#else
#define R 3
#endif
int main() { printf("ifd %d\n", R); return 0; }

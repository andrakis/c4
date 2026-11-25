#define X 1
#if defined X
#define R 1
#else
#define R 3
#endif
int main() { printf("idn %d\n", R); return 0; }

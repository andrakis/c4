#define X 1
#if defined(X)
#define R 1
#else
#define R 3
#endif
int main() { printf("ido %d\n", R); return 0; }

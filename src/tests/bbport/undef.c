#define A 1
#undef A
#ifdef A
#define R 3
#else
#define R 1
#endif
int main() { printf("ud %d\n", R); return 0; }

#define V 3
#if V >= 2 && (V * 2) == 6
#define R 1
#else
#define R 3
#endif
int main() { printf("iar %d\n", R); return 0; }

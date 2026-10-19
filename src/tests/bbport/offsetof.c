#include <stddef.h>
struct s { int a; char b; int c; };
int main() { printf("off %d\n", (int)offsetof(struct s, c) > 0); return 0; }

// libc.h's offsetof (M7b), inline: a whole-program probe cannot include
// libc.h, whose errno and stdin only a libc defines
#define offsetof(T, m) ((int)&((T *)0)->m)
struct s { int a; char b; int c; };
int main() { printf("off %d %d\n", (int)offsetof(struct s, c) > 0, (int)offsetof(struct s, a)); return 0; }

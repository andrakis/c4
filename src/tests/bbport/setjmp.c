#include <setjmp.h>
jmp_buf jb;
void f(void) { longjmp(jb, 3); }
int main() { int r; r = setjmp(jb); if (r == 0) f(); printf("sj %d\n", r); return 0; }

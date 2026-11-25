struct pt { int x; int y; };
int f(struct pt *p) { return p->x + p->y; }
int main() { printf("cl %d\n", f(&(struct pt){ 3, 4 })); return 0; }

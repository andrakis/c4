// c4fc F5/F6: structs and enums. Members are cell-sized, so a struct of
// two chars and an int is twenty-four bytes and the second char is at
// eight; a pointer to a struct steps by the struct's own size.
enum { RED, GREEN = 5, BLUE };
enum colour { PINK = -1, GREY };

struct P { int x; int y; char t; };
struct Q { char a; char b; int c; };
struct R { struct P p; int n; };

struct P gp;
struct P gpa[3];
struct Q *gq;

int s1(struct P *p) { return p->x + p->y + p->t; }
int s2(struct P *p, int v) { p->y = v; return p->x; }
int s3() { gp.x = 1; gp.y = 2; gp.t = 3; return gp.x + gp.y + gp.t; }
int s4() { return sizeof(struct P) + sizeof(struct Q) + sizeof(struct R); }
int s5(struct P *p) { return p + 1; }
int s6(struct P *p) { return p[2].y; }
int s7(struct P *p) { p++; return p->x; }
int s8() { struct P s; s.x = 4; return s.y; }
int s9() { struct P a[3]; a[1].x = 2; return a[1].x; }
int s10(struct R *r) { return r->p.y + r->n; }
int s11() { gpa[1].x = 9; return gpa[1].x; }
int s12(int c) { int r; r = 0;
   switch (c) { case RED: r = 1; break; case GREEN: r = 2; break; case BLUE: r = 3; }
   return r; }
int s13() { return PINK + GREY + RED + GREEN + BLUE; }

int main() { return s3() + s4() + s11() + s13(); }

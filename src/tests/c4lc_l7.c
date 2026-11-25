// c4lc L7: structs, typedef, unions, member access, do-while,
// compound assignment, block declarations. gcc is the oracle.
struct Point { int x; int y; };
struct Rect { struct Point *tl; struct Point *br; int tag; };
typedef struct Point PT;
typedef int word;

int more();
int chain();

struct Point origin;
struct Point corners[4];

int area (struct Rect *r) {
  return (r->br->x - r->tl->x) * (r->br->y - r->tl->y);
}

int main () {
  struct Point a;
  struct Point b;
  struct Rect r;
  PT *p;
  word w;
  int i;

  a.x = 3; a.y = 4;
  b.x = 13; b.y = 24;
  r.tl = &a; r.br = &b; r.tag = 7;
  printf("area %d tag %d\n", area(&r), r.tag);

  p = &a;
  p->x += 10;
  p->y <<= 2;
  printf("via ptr %d %d\n", a.x, a.y);

  origin.x = 100;
  for (i = 0; i < 4; i++) { corners[i].x = i * 10; corners[i].y = i + origin.x; }
  p = corners;
  p += 2;
  printf("corner2 %d %d third %d\n", p->x, p->y, corners[3].y);

  w = 0;
  do { w += 3; } while (w < 10);
  printf("dowhile %d\n", w);

  {
    int inner = a.x * 2;
    inner /= 2;
    printf("block %d\n", inner);
  }

  i = 5;
  i *= 3; i -= 5; i %= 7; i |= 8; i ^= 3; i &= 14;
  printf("compound %d\n", i);
  printf("chain %d\n", more());
  return 0;
}

union Cell { int i; char *s; };
struct Task { int id; char name[16]; int regs[4]; struct Task *next; };
struct Task tasks[3];

int chain () {
  struct Task *t;
  int n = 0;
  tasks[0].next = tasks + 1;
  tasks[1].next = tasks + 2;
  tasks[2].next = 0;
  t = tasks;
  while (t) { n++; t = t->next; }
  return n;
}

int more () {
  union Cell c;
  struct Task *t = tasks + 1;
  int k;
  t->id = 42;
  t->name[0] = 'T'; t->name[1] = 0;
  for (k = 0; k < 4; k++) t->regs[k] = k * k;
  c.i = 7;
  printf("task %d %s regs %d %d\n", t->id, t->name, t->regs[2], t->regs[3]);
  printf("union %d bigger %d\n", c.i, sizeof(struct Task) > sizeof(PT));
  return chain();
}

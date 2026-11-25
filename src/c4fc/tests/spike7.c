// c4fc F7: storage classes, prototypes, initialisers, constant
// expressions and struct array members.
//
// The data segment is three regions in this order: globals WITH an
// initialiser, then string literals, then globals without. A global
// initialised in a file whose first function holds a string still comes
// first, which is why this file has both.
enum { A = 1 + 2, B = A * 3, C, D = -1, E = (A + B) * 2 - 1 };
enum flavour { SALT = 2 * 4, PEPPER };

struct S { int n; char buf[8]; int tail; };
struct T { char name[16]; struct S inner; int count; };

int proto(char *s, int n);
static int shelper(int x);

int first = 7;
char *msg;
int table[3] = { 1, -2, 0x10 };
int part[4] = { A, B };
char word[] = "abc";
char pad[8];
static int hidden;
extern int shared;
int shared;
int last = B * 2;

static int shelper(int x) { return x + A; }
int proto(char *s, int n) { return s[0] + n; }
int use(struct S *s) { return s->n + s->buf[2] + s->tail; }
int deep(struct T *t) { return t->name[1] + t->inner.tail + t->count; }

int main()
{
  msg = "spike seven";
  hidden = 1;
  shared = 2;
  return proto(word, A) + shelper(B) + C + D + E + SALT + PEPPER
       + table[1] + part[2] + first + last + hidden + shared;
}

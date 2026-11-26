// t_virt2.cpp: new and delete, and inheritance without virtual. qtools'
// resource.h (struct DICTNODE : CACHENODE, a base reached through a
// derived pointer and the other way by a cast), new of a class with
// constructors, of a plain struct and of an int array, new[] and delete[]
// of a class with a constructor and destructor and of one with a
// constructor only, delete of 0, and a non-virtual destructor through
// delete. Every constructor and destructor prints, so the counts show.
extern "C" int printf (const char *, ...);

struct CACHENODE {
	void *ptr;
	CACHENODE *prev;
	CACHENODE *next;
	int lockCount;
};

struct DICTNODE : CACHENODE {
	unsigned int offset;
	unsigned int size;
	char type[3];
	char name[8];
};

static int live;

class Counted {
public:
	Counted () { id = ++made; live++; printf ("Counted %d\n", id); }
	Counted (int v) { id = v; live++; printf ("Counted (%d)\n", v); }
	~Counted () { live--; printf ("~Counted %d\n", id); }
	int id;
	static int made;
};

int Counted::made;

class Point {
public:
	Point () : x (1), y (2) {}
	Point (int a, int b) : x (a), y (b) {}
	int x, y;
	int sum () { return x + y; }
};

// a base with a destructor, and a derived class with members that have one
class Holder : public Counted {
public:
	Holder (int v) : Counted (v), inner (v + 100) { printf ("Holder %d\n", v); }
	~Holder () { printf ("~Holder %d\n", id); }
	Counted inner;
};

static void lock (CACHENODE *n) { n->lockCount++; }

int main (void)
{
	DICTNODE *d = new DICTNODE;
	CACHENODE *c;
	Point *pt = new Point (3, 4), *pts;
	Counted *one = new Counted (7), *many;
	int *nums = new int[5], i;
	Holder *h;
	d->ptr = 0;
	d->lockCount = 0;
	d->offset = 40;
	d->size = 2;
	lock (d);
	lock (d);
	c = d;
	printf ("lock %d, size %d, back %u\n", c->lockCount, (int)sizeof (DICTNODE), ((DICTNODE *)c)->offset);
	printf ("point %d\n", pt->sum ());
	for (i = 0; i < 5; i++)
		nums[i] = i * i;
	printf ("nums %d\n", nums[4] + nums[2]);
	many = new Counted[3];
	pts = new Point[2];
	printf ("live %d, pts %d %d\n", live, pts[0].sum (), pts[1].x);
	delete[] many;
	printf ("live %d\n", live);
	delete one;
	delete[] pts;
	delete pt;
	delete[] nums;
	delete d;
	one = 0;
	delete one;
	h = new Holder (5);
	printf ("live %d\n", live);
	delete h;
	printf ("live %d, made %d\n", live, Counted::made);
	return 0;
}

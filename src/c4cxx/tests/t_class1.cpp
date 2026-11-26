// t_class1.cpp: a Rect -- members, member functions inline and outside the
// class, implicit this, static members, a reference parameter, default
// arguments, bool, a file-scope const
extern "C" int printf (const char *, ...);

struct Rect {
	int x0, y0, x1, y1;
	Rect (int a, int b, int c, int d) { x0 = a; y0 = b; x1 = c; y1 = d; }
	int width () { return x1 - x0; }
	int height () { return y1 - y0; }
	int area ();
	bool contains (int x, int y) { return x >= x0 && x < x1 && y >= y0 && y < y1; }
	void offset (int dx, int dy = 0);
	Rect *self () { return this; }
	int sum () { return this->x0 + y0 + self ()->x1 + y1; }
	static int count;
	static int made () { return count; }
	static const int kCorners = 4;
};

int Rect::count = 0;

int Rect::area () { return width () * height (); }

void Rect::offset (int dx, int dy)
{
	x0 += dx;
	x1 += dx;
	y0 += dy;
	y1 += dy;
	count++;
}

void grow (Rect &r, int by)
{
	r.x1 += by;
	r.offset (0, by);
}

const int kSize = 4;
int table[kSize] = { 1, 2, 3, 4 };

int main ()
{
	Rect r (1, 2, 11, 7);
	Rect *p = &r;
	int corners[Rect::kCorners];
	printf ("w %d h %d area %d\n", r.width (), r.height (), p->area ());
	printf ("contains %d %d\n", r.contains (5, 5), p->contains (0, 0));
	r.offset (3);
	p->offset (-1, 2);
	printf ("%d %d %d %d count %d %d\n", r.x0, r.y0, r.x1, r.y1, Rect::count, Rect::made ());
	grow (r, 5);
	printf ("%d %d %d %d sum %d count %d\n", r.x0, r.y0, r.x1, r.y1, r.sum (), r.made ());
	printf ("size %d table %d corners %d\n", kSize, table[kSize - 1], (int)(sizeof corners / sizeof corners[0]));
	bool b = 5;
	bool c = false;
	printf ("bool %d %d %d\n", (int)b, (int)c, (int)sizeof (b));
	return 0;
}

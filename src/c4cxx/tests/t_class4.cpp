// t_class4.cpp: nested types (and X::Inner outside the class), enums and
// typedefs in a class, X::kFoo, a member returning a reference, a member
// calling one defined later in the class, a converting constructor
// (X x = 5), static members through an object, globals initialized at run
// time, `this` passed on, and a destructor reached through a member
extern "C" int printf (const char *, ...);

class Table {
public:
	enum { kRows = 3, kCols = 2 };
	typedef int CELL;
	struct Spot {
		int row, col;
	};
	CELL cells[kRows][kCols];
	Table () { clear (); }
	CELL &at (int r, int c) { return cells[r][c]; }
	CELL &atSpot (Spot s);
	void clear () { fill (0); }
	void fill (CELL v)
	{
		for (int r = 0; r < kRows; r++)
			for (int c = 0; c < kCols; c++)
				cells[r][c] = v + r * 10 + c;
	}
	static int tables;
};

int Table::tables = 2;

Table::CELL &Table::atSpot (Table::Spot s)
{
	return at (s.row, s.col);
}

struct Meters {
	int mm;
	Meters (int m) : mm (m * 1000) {}
	int cm () { return mm / 10; }
};

struct Node;
void visit (Node *n);

struct Node {
	int id;
	Node *next;
	Node (int i) : id (i), next (0) {}
	void walk () { visit (this); if (next) next->walk (); }
};

void visit (Node *n) { printf ("node %d\n", n->id); }

int base = 7;
int derived = base * 6;
int limit = Table::kRows * Table::kCols;
const int kScale = 3;
int scaled[kScale + 1] = { kScale, kScale * 2 };

struct Noisy {
	int n;
	Noisy (int k) : n (k) {}
	~Noisy () { printf ("noisy %d gone\n", n); }
};

struct Owner {
	Noisy a;
	Noisy b;
	Owner () : a (1), b (2) {}
	~Owner () { printf ("owner gone\n"); }
};

int main ()
{
	Table t;
	Table::Spot s = { 2, 1 };
	t.at (1, 1) = 99;
	t.atSpot (s) += 5;
	printf ("%d %d %d %d\n", t.at (0, 1), t.at (1, 1), t.atSpot (s), (int)sizeof (Table::CELL));
	printf ("rows %d cols %d tables %d %d\n", Table::kRows, t.kCols, Table::tables, t.tables);
	t.fill (100);
	printf ("%d\n", t.cells[2][0]);
	Meters m = 5;
	Meters k (2);
	printf ("cm %d %d\n", m.cm (), k.cm ());
	Node a (1), b (2), c (3);
	a.next = &b;
	b.next = &c;
	a.walk ();
	printf ("globals %d %d %d %d %d\n", base, derived, limit, scaled[0], scaled[1]);
	{
		Owner o;
		printf ("owner %d %d\n", o.a.n, o.b.n);
	}
	return 0;
}

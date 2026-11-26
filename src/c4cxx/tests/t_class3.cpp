// t_class3.cpp: references swapping, const Error&, a const reference to an
// rvalue, a function returning a reference, default arguments, anonymous
// unions and structs, extern "C"
extern "C" {
	int printf (const char *, ...);
	int c_helper (int x) { return x + 100; }
}

void swap (int &a, int &b)
{
	int t = a;
	a = b;
	b = t;
}

struct Error {
	int code;
	const char *where;
};

void report (const Error &e) { printf ("error %d at %s\n", e.code, e.where); }

int twice (const int &v) { return v * 2; }

int &pick (int *arr, int i) { return arr[i]; }

struct Value {
	int kind;
	union {
		int i;
		short s[2];
	};
	struct {
		char c, d;
	};
};

void show (const char *what, int n = 3, int m = -1) { printf ("%s %d %d\n", what, n, m); }

struct Box {
	int &ref;
	int own;
	Box (int &r) : ref (r), own (r + 1) {}
	void bump () { ref++; own++; }
};

int main ()
{
	int x = 1, y = 2;
	int arr[4] = { 5, 6, 7, 8 };
	Error e = { 42, "here" };
	Value v;
	int &rx = x;
	swap (x, y);
	printf ("%d %d %d\n", x, y, rx);
	rx = 9;
	printf ("x %d\n", x);
	report (e);
	printf ("twice %d %d\n", twice (x + 1), twice (y));
	pick (arr, 2) = 70;
	pick (arr, 3)++;
	printf ("%d %d %d\n", arr[2], arr[3], pick (arr, 0));
	v.kind = 1;
	v.i = 0x10002;
	v.c = 'a';
	v.d = 'b';
	printf ("%d %d %d %c%c %d\n", v.kind, v.s[0], v.s[1], v.c, v.d, (int)sizeof (Value));
	show ("a");
	show ("b", 4);
	show ("c", 5, 6);
	printf ("%d\n", c_helper (1));
	{
		Box b (y);
		b.bump ();
		printf ("box %d %d %d\n", y, b.ref, b.own);
	}
	return 0;
}

// t_class2.cpp: constructors writing to a log, destructors at the end of
// blocks and at return, break and continue, a stack class, init lists in
// declaration order, members with constructors, a global object
extern "C" int printf (const char *, ...);

class Log {
public:
	const char *name;
	Log (const char *n) { name = n; printf ("+%s\n", name); }
	~Log () { printf ("-%s\n", name); }
};

class Stack {
	int items[16];
	int n;
public:
	Stack () : n (0) { printf ("stack made\n"); }
	~Stack () { printf ("stack gone with %d\n", n); }
	void push (int v) { items[n++] = v; }
	int pop () { return items[--n]; }
	bool empty () { return n == 0; }
};

Log global ("global");

int f (int k)
{
	Log a ("f.a");
	if (k == 1) {
		Log b ("f.b");
		return 10;
	}
	for (int i = 0; i < 3; i++) {
		Log c ("f.loop");
		if (i == k)
			break;
		if (i == 0)
			continue;
		printf ("loop %d\n", i);
	}
	switch (k) {
	case 2: {
		Log d ("f.case");
		break;
	}
	default:
		break;
	}
	return k * 2;
}

struct Pair {
	Log first;
	Log second;
	int tag;
	Pair () : first ("pair.first"), second ("pair.second"), tag (7) { printf ("pair made %d\n", tag); }
};

struct Counter {
	int n;
	Counter () { n = 0; printf ("counter\n"); }
};

// no constructor of its own: one is made for its member
struct Holder {
	Counter c;
	int extra;
};

void early (int k)
{
	Log a ("early");
	if (k)
		return;
	printf ("early went on\n");
}

int main ()
{
	printf ("main\n");
	{
		Stack s;
		s.push (1);
		s.push (2);
		s.push (3);
		while (!s.empty ())
			printf ("pop %d\n", s.pop ());
		s.push (9);
	}
	printf ("f %d\n", f (1));
	printf ("f %d\n", f (2));
	printf ("f %d\n", f (5));
	{
		Pair p;
		printf ("tag %d\n", p.tag);
	}
	{
		Holder h;
		h.extra = 3;
		printf ("holder %d %d\n", h.c.n, h.extra);
	}
	early (1);
	early (0);
	printf ("end\n");
	return 0;
}

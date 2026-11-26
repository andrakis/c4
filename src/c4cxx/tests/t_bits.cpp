// t_bits.cpp: what M6 needed of Blood beyond M5. Bit-fields in a struct
// as Blood's XSPRITE has them, and in a class with member functions that
// set them; through pointers, compound assignments, ++ and --, read back
// signed and unsigned. A parameter of function type, which C makes a
// pointer (mact's control.h): c4cxx writes it as one, for c4lc.
extern "C" int printf (const char *, ...);

struct XSPRITE {
	signed int reference : 15;
	unsigned int state : 1;
	unsigned int busy : 17;
	unsigned int txID : 10;
	unsigned int rxID : 10;
	unsigned int command : 8;
	unsigned int triggerOn : 1;
	unsigned int triggerOff : 1;
	unsigned int wave : 2;
	unsigned int busyTime : 12;
	unsigned int waitTime : 12;
	unsigned int restState : 1;
	unsigned int interruptable : 1;
	int data1 : 16;
};

XSPRITE xsprite[4];

class Switch {
public:
	Switch (int r) { x.reference = r; x.state = 0; x.busy = 0; }
	void Toggle () { x.state ^= 1; x.busy += 0x8000; }
	int Busy () { return x.busy; }
	int State () { return x.state; }
	XSPRITE x;
};

static void tick (int n) { printf ("tick %d\n", n); }
static void tock (int n) { printf ("tock %d\n", n); }

static void each (void f (int), int n)
{
	for (int i = 0; i < n; i++)
		f (i);
}

static void CONTROL_Center (void center (void), void corner (int))
{
	center ();
	corner (2);
}

static void hello (void) { printf ("hello\n"); }

int main ()
{
	XSPRITE *p = &xsprite[2];
	Switch s (-7);
	p->reference = -5;
	p->txID = 1023;
	p->rxID = p->txID + 1;
	p->command = 300;
	p->wave = 2;
	p->wave++;
	p->busyTime = 4095;
	p->busyTime += 2;
	p->data1 = -30000;
	p->data1 -= 5000;
	printf ("%d %d %d %d\n", p->reference, p->txID, p->rxID, p->command);
	printf ("%d %d %d\n", p->wave, p->busyTime, p->data1);
	printf ("size %d\n", (int)sizeof (XSPRITE));
	s.Toggle ();
	s.Toggle ();
	s.Toggle ();
	printf ("switch %d %d %d\n", s.State (), s.Busy (), s.x.reference);
	each (tick, 2);
	each (tock, 1);
	CONTROL_Center (hello, tock);
	return 0;
}

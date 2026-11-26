// t_virt1.cpp: a Widget tree in the shape of Blood's gui.h, painted and
// deleted through base pointers. Pure virtuals, a virtual destructor
// defined in the class, a key function out of the class (Label::Paint), a
// class whose virtuals are all inline (HeadWidget), overrides two levels
// down, an inherited virtual not overridden, Base::f () from an override,
// virtual calls from constructors and destructors, inherited data and
// member functions, and derived-to-base conversions: assignment, argument,
// return, reference.
extern "C" int printf (const char *, ...);

enum MODAL_RESULT { MODAL_RESULT_0, MODAL_RESULT_1, MODAL_RESULT_2 };

class Widget
{
public:
	Widget (int a1, int a2, int a3, int a4) : at0 (a1), at4 (a2), at8 (a3), atc (a4), at10 (0), at14 (0), at18 (0), at1c (0)
	{
		printf ("Widget (%d, %d) is a %s\n", at0, at4, Name ());
	}
	virtual ~Widget () { printf ("~Widget (%d, %d)\n", at0, at4); }

	int at0;
	int at4;
	int at8;
	int atc;
	Widget *at10;
	Widget *at14;
	Widget *at18;
	char at1c;
	int Inside (int x, int y) { return x >= at0 && y >= at4 && x < at0 + at8 && y < at4 + atc; }
	virtual void Paint (int, int, char) = 0;
	virtual void HandleEvent (int) = 0;
	virtual void EndModal (MODAL_RESULT a1) { printf ("Widget::EndModal (%d)\n", a1); if (at18) at18->EndModal (a1); }
	virtual const char *Name (void) { return "Widget"; }
};

class HeadWidget : public Widget
{
public:
	HeadWidget () : Widget (0, 0, 0, 0) {}
	virtual void Paint (int, int, char) {}
	virtual void HandleEvent (int) {}
	virtual const char *Name (void) { return "HeadWidget"; }
};

class Label : public Widget
{
public:
	Label (int, int, const char *);
	const char *text;
	virtual void Paint (int, int, char);
	virtual void HandleEvent (int e) { printf ("Label '%s' takes %d\n", text, e); }
	virtual const char *Name (void) { return "Label"; }
};

Label::Label (int x, int y, const char *s) : Widget (x, y, 10, 10)
{
	text = s;
}

void Label::Paint (int x, int y, char c)
{
	printf ("Label '%s' at %d, %d (%c)\n", text, x + at0, y + at4, c);
}

class Container : public Widget
{
public:
	Container (int a1, int a2, int a3, int a4) : Widget (a1, a2, a3, a4)
	{
		at10 = at14 = 0;
		count = 0;
	}
	virtual ~Container ();
	int count;
	void Insert (Widget *w)
	{
		w->at18 = this;
		w->at14 = 0;
		if (at14)
			at14->at14 = w;
		else
			at10 = w;
		at14 = w;
		count++;
	}
	virtual void Paint (int x, int y, char c)
	{
		Widget *w;
		printf ("%s at %d, %d paints %d\n", Name (), x + at0, y + at4, count);
		for (w = at10; w; w = w->at14)
			w->Paint (x + at0, y + at4, c);
	}
	virtual void HandleEvent (int e)
	{
		for (Widget *w = at10; w; w = w->at14)
			if (w->Inside (e, e))
				w->HandleEvent (e);
	}
	virtual void EndModal (MODAL_RESULT a1) { printf ("Container::EndModal (%d)\n", a1); result = a1; }
	virtual const char *Name (void) { return "Container"; }
	MODAL_RESULT result;
};

Container::~Container ()
{
	Widget *w = at10, *n;
	printf ("~Container of %d (a %s)\n", count, Name ());
	while (w) {
		n = w->at14;
		delete w;
		w = n;
	}
}

class Panel : public Container
{
public:
	Panel (int a1, int a2, int a3, int a4, int b) : Container (a1, a2, a3, a4), border (b) {}
	int border;
	virtual void Paint (int x, int y, char c)
	{
		printf ("Panel border %d\n", border);
		Container::Paint (x, y, c);
	}
	virtual const char *Name (void) { return "Panel"; }
	~Panel () { printf ("~Panel %d\n", border); }
};

class Button : public Widget
{
public:
	Button (int a1, int a2, int a3, int a4, MODAL_RESULT r) : Widget (a1, a2, a3, a4), result (r) {}
	MODAL_RESULT result;
	virtual void Paint (int x, int y, char c) { printf ("Button %d at %d, %d (%c)\n", result, x + at0, y + at4, c); }
	virtual void HandleEvent (int) { EndModal (result); }
	virtual const char *Name (void) { return "Button"; }
};

class TextButton : public Button
{
public:
	TextButton (int a1, int a2, int a3, int a4, const char *s, MODAL_RESULT r) : Button (a1, a2, a3, a4, r)
	{
		text = s;
	}
	const char *text;
	virtual void Paint (int x, int y, char c)
	{
		printf ("TextButton '%s': ", text);
		Button::Paint (x, y, c);
	}
	virtual const char *Name (void) { return "TextButton"; }
};

static Widget *first (Container *c) { return c->at10; }
static Container *as_container (Panel *p) { return p; }
static void paint_it (Widget &w, int at) { w.Paint (at, at, '#'); }
static int area (Widget *w) { return w->at8 * w->atc; }

Panel *make (void)
{
	Panel *p = new Panel (5, 5, 100, 100, 2);
	Container *inner = new Container (10, 20, 50, 50);
	p->Insert (new Label (1, 1, "title"));
	p->Insert (new TextButton (2, 30, 20, 10, "&OK", MODAL_RESULT_1));
	inner->Insert (new TextButton (3, 3, 20, 10, "&Cancel", MODAL_RESULT_2));
	inner->Insert (new HeadWidget);
	p->Insert (inner);
	return p;
}

int main (void)
{
	Panel *p = make ();
	Widget *w = p;
	Container *c;
	HeadWidget head;
	w->Paint (0, 0, '*');
	printf ("area %d, first is a %s\n", area (p), first (p)->Name ());
	paint_it (*first (p), 7);
	paint_it (head, 1);
	c = as_container (p);
	printf ("inside %d %d\n", c->Inside (10, 10), c->Inside (200, 10));
	// a button's event ends its container's modal loop
	first (p)->at14->HandleEvent (3);
	printf ("result %d\n", c->result);
	w->EndModal (MODAL_RESULT_0);
	head.EndModal (MODAL_RESULT_2);
	c->HandleEvent (12);
	delete w;
	printf ("done\n");
	return 0;
}

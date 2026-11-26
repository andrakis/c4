// 2_view.cpp: a saver whose key function is in its own unit (so is its
// vtable), joining the list before main
#include "loadsave.h"

class ViewLoadSave : public LoadSave {
public:
	ViewLoadSave () { printf ("ViewLoadSave joins as %c\n", tag); }
	virtual void Load (void);
	virtual void Save (void);
	int frames;
};

void ViewLoadSave::Load (void)
{
	printf ("ViewLoadSave::Load %c %d\n", tag, frames);
}

void ViewLoadSave::Save (void)
{
	frames = 30;
	printf ("ViewLoadSave::Save %c\n", tag);
	LoadSave::Save ();
}

static ViewLoadSave myLoadSave;

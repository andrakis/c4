// 1_loadsave.cpp: LoadSave's head and its key function (Load), so its
// vtable is here; main walks the list the other units' globals joined
#include "loadsave.h"

LoadSave LoadSave::head (7);
int LoadSave::count;

void LoadSave::Load (void)
{
	printf ("LoadSave::Load %c\n", tag);
}

void LoadSave::Save (void)
{
	printf ("LoadSave::Save %c\n", tag);
}

void LoadSave::LoadGame (void)
{
	for (LoadSave *p = head.next; p != &head; p = p->next)
		p->Load ();
}

void LoadSave::SaveGame (void)
{
	for (LoadSave *p = head.next; p != &head; p = p->next)
		p->Save ();
}

class MyLoadSave : public LoadSave {
public:
	virtual void Load (void) { printf ("MyLoadSave::Load %c\n", tag); }
};

static MyLoadSave myLoadSave;

int main (void)
{
	printf ("main: %d savers, size %d\n", LoadSave::count, (int)sizeof (LoadSave));
	LoadSave::SaveGame ();
	LoadSave::LoadGame ();
	return 0;
}

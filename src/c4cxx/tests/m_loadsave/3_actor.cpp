// 3_actor.cpp: two savers with all their functions in the class (a static
// vtable in this unit), the second overriding only Save
#include "loadsave.h"

class ActorLoadSave : public LoadSave {
public:
	virtual void Load (void) { printf ("ActorLoadSave::Load %c\n", tag); }
	virtual void Save (void) { printf ("ActorLoadSave::Save %c\n", tag); }
};

class AILoadSave : public LoadSave {
public:
	AILoadSave () { printf ("AILoadSave joins as %c\n", tag); }
	virtual void Save (void) { printf ("AILoadSave::Save %c\n", tag); }
};

static ActorLoadSave myLoadSave;
static AILoadSave aiLoadSave;

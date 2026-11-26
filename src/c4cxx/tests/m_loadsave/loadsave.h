// loadsave.h: Blood-RE's LoadSave (loadsave.h), packed as Blood was built:
// a list of savers built before main by the constructors of globals in
// several units. The head is built by LoadSave (int) in 1_loadsave.cpp;
// every saver's LoadSave () reads head.prev, so the head's unit must come
// first in the link, which test-cxx.sh's order (the files' names) keeps.
extern "C" int printf (const char *, ...);

#pragma pack(push, 1)
class LoadSave {
public:
	static LoadSave head;
	static int count;
	LoadSave *prev;
	LoadSave *next;
	char tag;
	LoadSave ()
	{
		prev = head.prev;
		prev->next = this;
		next = &head;
		next->prev = this;
		tag = 'a' + count++;
	}
	LoadSave (int dummy)
	{
		next = prev = this;
		tag = '0' + dummy;
		printf ("head built\n");
	}
	virtual void Load (void);
	virtual void Save (void);
	static void LoadGame (void);
	static void SaveGame (void);
};
#pragma pack(pop)

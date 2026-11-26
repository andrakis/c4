// t_virt3.cpp: globals as Blood's AI and menus have them. An AISTATE
// initialized by a list holding a value known only at run time (a seq
// client registered by another global's initializer); a global array of
// menu items, each built before main and destroyed after it, last first;
// default arguments in a header's declaration and not the definition's;
// an overload chosen by a ?: of two enumerators (evPost's case); a static
// const member defined outside its class (messages.cpp's kCheatFlagsNone).
extern "C" int printf (const char *, ...);

static int nClients;
static int seqRegisterClient (void (*f) (int)) { printf ("client %d\n", nClients); return nClients++; }
static void BiteSeqCallback (int) {}
static void SlashSeqCallback (int) {}

struct AISTATE {
	int at0;
	int at4;	// a seq client
	int at8;
	void (*enter) (int);
	AISTATE *next;
};

static int nBiteClient = seqRegisterClient (BiteSeqCallback);
static int nSlashClient = seqRegisterClient (SlashSeqCallback);
AISTATE batPonder = { 1, -1, 30, 0, 0 };
AISTATE batBite = { 7, nBiteClient, 60, 0, &batPonder };
AISTATE batSlash = { 8, nSlashClient, 90, BiteSeqCallback, &batBite };

class CGameMenuItem {
public:
	CGameMenuItem () { at4 = 0; printf ("item %d\n", ++made); }
	virtual ~CGameMenuItem () { printf ("~item %d\n", at4); }
	virtual void Draw (void) = 0;
	virtual int Event (int &e) { e++; return 0; }
	int at4;
	static int made;
	static const unsigned long kFlags;
};

int CGameMenuItem::made;
const unsigned long CGameMenuItem::kFlags = 6;

class CGameMenuItemChain : public CGameMenuItem {
public:
	CGameMenuItemChain () { at4 = made * 10; }
	virtual void Draw (void) { printf ("chain %d\n", at4); }
	virtual int Event (int &e) { CGameMenuItem::Event (e); return at4 + e; }
};

CGameMenuItemChain itemEpisodes[3];

enum COMMAND_ID { COMMAND_ID_0, COMMAND_ID_1 };
enum CALLBACK_ID { CALLBACK_ID_0, CALLBACK_ID_9 = 9 };

void evPost (int nIndex, int nType, unsigned long nDelta, COMMAND_ID command);
void evPost (int nIndex, int nType, unsigned long nDelta, CALLBACK_ID callback);
void aiPlay3DSound (int nSprite, int a2, int a3, int a4 = -1);

void evPost (int nIndex, int nType, unsigned long nDelta, COMMAND_ID command)
{
	printf ("evPost %d %d %lu command %d\n", nIndex, nType, nDelta, command);
}

void evPost (int nIndex, int nType, unsigned long nDelta, CALLBACK_ID callback)
{
	printf ("evPost %d %d %lu callback %d\n", nIndex, nType, nDelta, callback);
}

void aiPlay3DSound (int nSprite, int a2, int a3, int a4)
{
	printf ("sound %d %d %d %d\n", nSprite, a2, a3, a4);
}

int main (void)
{
	AISTATE *s;
	int i, e = 1, on = 1;
	for (s = &batSlash; s; s = s->next)
		printf ("state %d client %d time %d enter %d\n", s->at0, s->at4, s->at8, s->enter != 0);
	for (i = 0; i < 3; i++) {
		CGameMenuItem *item = &itemEpisodes[i];
		item->Draw ();
		printf ("event %d\n", item->Event (e));
	}
	evPost (3, 3, 120, on ? COMMAND_ID_1 : COMMAND_ID_0);
	evPost (3, 3, 120, CALLBACK_ID_9);
	aiPlay3DSound (1, 1600, 2);
	aiPlay3DSound (1, 1600, 2, 5);
	printf ("flags %lu\n", CGameMenuItem::kFlags);
	return 0;
}

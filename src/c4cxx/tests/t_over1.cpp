// t_over1.cpp: overloaded functions, as Blood has them -- evPost on two
// enums, sndStartSample (char *, ...) and (ulong, ...) with default
// arguments, FindSector with 3 and 4 arguments, LevelWarp () -- and the
// ranks that choose: exact, promotion, conversion, const, a null pointer
extern "C" int printf (const char *, ...);

typedef unsigned long ulong;
typedef unsigned char BOOL;

enum COMMAND_ID { kCmdOff = 0, kCmdOn = 1, kCmdToggle = 2 };
enum CALLBACK_ID { kCallbackNone = -1, CALLBACK_ID_17 = 17 };

void evPost (int nIndex, int nType, unsigned long nDelta, COMMAND_ID command)
{
	printf ("evPost command %d %d %lu %d\n", nIndex, nType, nDelta, (int)command);
}

void evPost (int nIndex, int nType, unsigned long nDelta, CALLBACK_ID callback)
{
	printf ("evPost callback %d %d %lu %d\n", nIndex, nType, nDelta, (int)callback);
}

void sndStartSample (char *pzSound, int nVolume, int nChannel = -1)
{
	printf ("sample named %s %d %d\n", pzSound, nVolume, nChannel);
}

void sndStartSample (ulong nSound, int nVolume, int nChannel = -1, BOOL bLoop = 0)
{
	printf ("sample %lu %d %d %d\n", nSound, nVolume, nChannel, bLoop);
}

BOOL FindSector (int nX, int nY, int nZ, int *nSector)
{
	*nSector = nX + nY + nZ;
	return 1;
}

BOOL FindSector (int nX, int nY, int *nSector)
{
	*nSector = nX * nY;
	return 0;
}

void LevelWarp (int nEpisode, int nLevel) { printf ("warp to %d %d\n", nEpisode, nLevel); }
void LevelWarp () { printf ("warp on\n"); }

void kind (int) { printf ("int\n"); }
void kind (long) { printf ("long\n"); }
void kind (double) { printf ("double\n"); }
void kind (char *) { printf ("char *\n"); }
void kind (const char *) { printf ("const char *\n"); }
void kind (void *) { printf ("void *\n"); }

void ref (int &x) { printf ("int & %d\n", x); }
void ref (const int &x) { printf ("const int & %d\n", x); }

struct Mixer {
	int level;
	Mixer () : level (0) {}
	void set (int v) { level = v; }
	void set (int v, int scale) { level = v * scale; }
	void set (double v) { level = (int)(v * 100); }
	static int clip (int v) { return v > 99 ? 99 : v; }
	static int clip (int v, int hi) { return v > hi ? hi : v; }
};

int main ()
{
	int s, r;
	char name[] = "boom";
	const char *cname = "const";
	char c = 'x';
	short sh = 3;
	float f = 1.5f;
	evPost (1, 3, 0, kCmdOn);
	evPost (2, 6, 120, CALLBACK_ID_17);
	evPost (3, 6, 5u, (COMMAND_ID)kCmdToggle);
	sndStartSample (name, 64);
	sndStartSample (name, 64, 2);
	sndStartSample (3002, 80);
	sndStartSample (3003ul, 80, 1, 1);
	r = FindSector (1, 2, 3, &s);
	printf ("%d %d\n", r, s);
	r = FindSector (4, 5, &s);
	printf ("%d %d\n", r, s);
	LevelWarp (2, 3);
	LevelWarp ();
	kind (1);
	kind (1L);
	kind (c);
	kind (sh);
	kind (f);
	kind (2.5);
	kind (name);
	kind (cname);
	kind ("literal");
	kind (&s);
	kind (kCmdOn);
	ref (s);
	ref (cname[0]);
	ref (s + 1);
	Mixer m;
	m.set (5);
	printf ("level %d\n", m.level);
	m.set (5, 3);
	printf ("level %d\n", m.level);
	m.set (0.25);
	printf ("level %d\n", m.level);
	printf ("clip %d %d %d\n", Mixer::clip (120), Mixer::clip (120, 50), m.clip (7));
	return 0;
}

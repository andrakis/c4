// w_watcom.cpp: Watcom's layouts, as Blood's host build gives them to g++
// (-fshort-enums -mms-bitfields -funsigned-char; test-cxx.sh gives a
// w_*.cpp those flags everywhere). An enum is the smallest type that holds
// it (GAMEOPTIONS), a bit-field has a unit of its type (INPUT), plain char
// is unsigned (cache1d's group 255). c4cxx works out sizes itself where a
// constant needs them (an array's length), so it must lay out as gcc does.
extern "C" int printf (const char *, ...);

enum GAMETYPE { kGameTypeSinglePlayer, kGameTypeCoop, kGameTypeBloodBath, kGameTypeTeams };
enum SIGNED { kMinus = -1, kPlus = 1 };
enum BIG { kBig = 70000 };

#pragma pack(push, 1)
struct GAMEOPTIONS {
	GAMETYPE nGameType;
	char nDifficulty;
	int nEpisode;
	SIGNED nSign;
	BIG nBig;
	short nLevel;
};
struct INPUT {
	unsigned char syncFlags;
	short forward;
	short turn;
	short strafe;
	unsigned int run : 1;
	unsigned int jump : 1;
	unsigned int crouch : 1;
	unsigned int newWeapon : 8;
	unsigned char key : 1;
	unsigned char spin : 1;
	unsigned short look : 3;
	unsigned int rest : 12;
};
#pragma pack(pop)

struct Unpacked {
	char c;
	double d;
	unsigned int f : 3;
};

const int kOptionsSize = sizeof (GAMEOPTIONS);
static char saved[kOptionsSize * 2];
static char frame[sizeof (INPUT) + sizeof (Unpacked)];

int main ()
{
	char group = 255;
	char c = 200;
	INPUT in;
	in.newWeapon = 0xff;
	in.look = 7;
	printf ("sizes %d %d %d\n", (int)sizeof (GAMETYPE), (int)sizeof (SIGNED), (int)sizeof (BIG));
	printf ("options %d input %d unpacked %d\n", (int)sizeof (GAMEOPTIONS), (int)sizeof (INPUT), (int)sizeof (Unpacked));
	printf ("arrays %d %d\n", (int)sizeof saved, (int)sizeof frame);
	printf ("char %d %d %d\n", group, c > 100, (c + 100) / 2);
	printf ("input %d %d\n", in.newWeapon, in.look);
	return group == 255 ? 0 : 1;
}

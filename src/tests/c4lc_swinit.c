// c4lc_swinit.c -- two initializers as Shadow Warrior writes them
// (games/sw):
//
// A static local's initializer naming the array itself, as its state
// rings do (game.c's s_BonusGrab):
//   static STATE s_BonusGrab[] = { {..., &s_BonusGrab[1]}, ... };
// and the size of an array its unit defines, after a block-scope
// `extern STATE s_BasicExp[];` (weapon.c), which declares it again
// without one:
//   saveable_data saveable_weapon_data[] = { { &s_BasicExp, sizeof(s_BasicExp) } };
// Exits 42.
struct st { int v; struct st *next; };

int f()
{
	static struct st ring[3] = { { 1, &ring[1] }, { 2, &ring[2] }, { 39, &ring[0] } };
	// and a table of other static locals, by name and by &name, as
	// weapon.c's LavaShrapTable is
	static int a1[2] = { 1, 2 };
	static int b1[2] = { 3, 4 };
	static int *tb[] = { a1, b1 };
	static int *td[] = { (int *)&b1 };
	// 39 + 2 + 1, and 4 + 3 - 7
	return ring[0].next->next->v + ring[2].next->next->v + ring[2].next->v + tb[1][1] + td[0][0] - 7;
}

int table[] = { 1, 2, 3, 4 };

int g()
{
	extern int table[];
	return table[1];
}

struct sd { void *p; int n; };
struct sd sd[1] = { { (void *)&table, sizeof (table) } };

int main()
{
	return f() + (sd[0].n == 4 * sizeof (int)) - 1 + g() - 2 + (sd[0].p == (void *)table) - 1;
}

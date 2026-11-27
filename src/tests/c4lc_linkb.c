// c4lc_linkb.c: c4lc_linka.c's other unit
static int helper (void);
int from_a (void);

int helper (void)
{
	return 3;
}

int main (void)
{
	printf ("a %d b %d\n", from_a (), helper ());
	return 0;
}

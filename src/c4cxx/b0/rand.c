// Watcom's rand, for B0 (blood-b0.sh): DOS Blood drew from it, as c4's
// builds draw from C4IX's libc's (the same generator), and glibc's draws
// another sequence. Blood calls rand in weapon.cpp, view.cpp, fire.cpp and
// for network.cpp's first checksum.
static unsigned int rand_next = 1;

int rand (void)
{
	rand_next = rand_next * 1103515245u + 12345u;
	return (int)((rand_next >> 16) & 0x7fff);
}

void srand (unsigned int seed)
{
	rand_next = seed;
}

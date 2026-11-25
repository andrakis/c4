// c4lc_mainproto.c -- main's prototype, in a unit without main. It has to compile:
// c4lc took the extern's number for a label, and with more externs than labels
// stopped at "string:word out of range". test-c4lc-opassign.
int unsetenv(char *name);
void srand(unsigned int seed);
int main(int, char **);
int g(void) { return 1; }

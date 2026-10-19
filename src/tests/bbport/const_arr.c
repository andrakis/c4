static const char *const words[] = { "one", "two", "three" };
int main() { printf("ca %s %d\n", words[2], (int)(sizeof(words)/sizeof(words[0]))); return 0; }

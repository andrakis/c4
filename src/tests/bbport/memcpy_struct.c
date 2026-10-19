struct pt { int x; int y; };
int main() { struct pt a; struct pt b; a.x = 1; a.y = 2; memcpy(&b, &a, sizeof(struct pt)); printf("mcs %d\n", b.y); return 0; }

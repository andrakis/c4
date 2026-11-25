struct s { int a; char b[6]; };
int main() { printf("st %d %d\n", (int)sizeof(int) >= 4, (int)sizeof(struct s) >= 10); return 0; }

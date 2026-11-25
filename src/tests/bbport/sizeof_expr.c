struct s { char a[10]; int b; };
int main() { struct s x; printf("sz %d %d\n", (int)sizeof x.a, (int)sizeof(struct s) >= 14); return 0; }

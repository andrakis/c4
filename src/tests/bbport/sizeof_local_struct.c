struct s { int a; int b; };
int main() { struct s v; printf("sls %d\n", (int)sizeof(v) >= 8); return 0; }

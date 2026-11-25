struct s { int a; int b; };
int main() { struct s *p; p = 0; printf("sd %d\n", (int)sizeof(*p) >= 8); return 0; }

struct b { unsigned a : 3; unsigned c : 5; };
int main() { struct b x; x.a = 5; x.c = 17; printf("bf %d %d\n", x.a, x.c); return 0; }

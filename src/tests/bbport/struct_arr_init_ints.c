struct pt { int x; int y; };
struct pt ps[2] = { { 1, 2 }, { 3, 4 } };
int main() { printf("sai %d\n", ps[1].y); return 0; }

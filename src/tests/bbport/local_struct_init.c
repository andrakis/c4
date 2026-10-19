struct pt { int x; int y; };
int main() { struct pt p = { 3, 4 }; printf("lsi %d\n", p.x + p.y); return 0; }

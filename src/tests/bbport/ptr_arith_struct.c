struct pt { int x; int y; };
int main() { struct pt a[3]; struct pt *p; p = a; p++; p->x = 4; printf("pas %d %d\n", a[1].x, (int)(p - a)); return 0; }

struct pt { int x; int y; };
int main() { struct pt a[3]; a[2].y = 9; printf("asl %d\n", a[2].y); return 0; }

int twice(int x) { return 2 * x; }
int main() { int (*fp)(int); fp = twice; printf("fp %d\n", fp(21)); return 0; }

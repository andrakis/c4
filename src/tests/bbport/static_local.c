int counter() { static int n = 10; n = n + 1; return n; }
int main() { counter(); counter(); printf("static %d\n", counter()); return 0; }

struct ap { char *name; int f; };
struct ap a[2] = { { "a", 1 }, { "b", 2 } };
int main() { printf("sis %s %d\n", a[1].name, a[1].f); return 0; }

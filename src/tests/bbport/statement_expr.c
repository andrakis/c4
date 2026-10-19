#define MAX(a, b) ({ int _a = (a); int _b = (b); _a > _b ? _a : _b; })
int main() { printf("se %d\n", MAX(3, 8)); return 0; }

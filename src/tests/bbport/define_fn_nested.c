#define SQ(x) ((x) * (x))
#define QUAD(x) SQ(SQ(x))
int main() { printf("dfn %d\n", QUAD(2)); return 0; }

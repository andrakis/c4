// c4fc F5/F6: pointer scaling and arrays. A pointer steps by what it
// points AT, and a step of one emits no multiply -- char * walks byte
// by byte with no MUL in sight.
int gi[4];
char gc[8];
int *gp;

int p1(int *p, int i) { return p + i; }
int p2(char *p, int i) { return p + i; }
int p3(int *p, int *q) { return p - q; }
int p4(char *p, char *q) { return p - q; }
int p5(int *p, int i) { return p[i]; }
int p6(char *p, int i) { return p[i]; }
int p7(int *p) { return *(p + 1); }
int p8(int *p) { p++; return *p; }
int p9(char *p) { p++; p--; return *p; }
int p10(int **pp) { return **pp; }

int a1(int n) { int a[4]; int t; a[0] = n; t = a[1]; return t; }
int a2(int n) { char b[8]; b[0] = n; return b[1]; }
int a3(int n) { int a[3]; int i, s; s = 0; for (i = 0; i < 3; i++) a[i] = i * n;
                for (i = 0; i < 3; i++) s = s + a[i]; return s; }
int a4() { gi[2] = 7; gc[3] = 8; gp = gi; return gi[2] + gc[3] + *gp; }
int a5() { return sizeof(int) + sizeof(char) + sizeof(int *) + sizeof(char *); }

int main() { return a4() + a5(); }

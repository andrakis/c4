#define STR(x) #x
#define CAT(a, b) a##b
int main() { int CAT(fo, o) = 3; printf("pp %s %d\n", STR(hello), foo); return 0; }

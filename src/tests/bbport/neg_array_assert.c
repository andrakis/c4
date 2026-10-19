struct G { int a[4]; };
struct BUG_G_too_big { char BUG_G_too_big[sizeof(struct G) <= 64 ? 1 : -1]; };
int main() { printf("nas ok\n"); return 0; }

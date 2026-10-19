struct globals { int a; char buf[16]; };
struct globals *ptr_to_globals;
#define G (*ptr_to_globals)
#define a_ (G.a)
int main() { ptr_to_globals = malloc(sizeof(G)); a_ = 4; memcpy(G.buf, "gg", 3); printf("ptg %d %s\n", a_, G.buf); return 0; }

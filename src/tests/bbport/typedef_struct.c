typedef struct node { int v; struct node *next; } node_t;
int main() { node_t a; node_t b; a.v = 1; a.next = &b; b.v = 2; printf("ts %d\n", a.next->v); return 0; }

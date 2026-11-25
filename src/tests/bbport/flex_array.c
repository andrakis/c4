struct s { int n; char data[]; };
int main() { struct s *p; p = malloc(sizeof(struct s) + 8); p->n = 2; p->data[0] = 'x'; p->data[1] = 0;
  printf("flex %d %s\n", p->n, p->data); return 0; }

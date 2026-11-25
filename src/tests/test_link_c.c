// C4 Test: linking with L7 structs, module C (main). Pairs with
// test_link_d.c; both repeat the struct definition, as separate
// units must (there is no preprocessor).
struct Node { int val; struct Node *next; };

struct Node *list_push (struct Node *head, struct Node *slot, int val);
int list_sum (struct Node *head);
int list_len (struct Node *head);

int main () {
  struct Node pool[8];
  struct Node *head;
  int i;
  head = 0;
  for (i = 1; i <= 5; i++) head = list_push(head, pool + i, i * 10);
  printf("sum %d len %d head %d\n", list_sum(head), list_len(head), head->val);
  return 0;
}

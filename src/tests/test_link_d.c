// C4 Test: linking with L7 structs, module D (library).
struct Node { int val; struct Node *next; };

void __attribute__((constructor)) d_constructor () {
  printf("d: constructor\n");
}

struct Node *list_push (struct Node *head, struct Node *slot, int val) {
  slot->val = val;
  slot->next = head;
  return slot;
}

int list_sum (struct Node *head) {
  int n = 0;
  while (head) { n += head->val; head = head->next; }
  return n;
}

int list_len (struct Node *head) {
  int n = 0;
  do { if (!head) return n; n++; head = head->next; } while (1);
  return 0;
}

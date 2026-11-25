typedef int (*op_t)(int, int);
int add(int a, int b) { return a + b; }
int sub(int a, int b) { return a - b; }
op_t ops[2] = { add, sub };
int main() { op_t f; f = ops[1]; printf("td %d %d\n", ops[0](3, 4), f(9, 2)); return 0; }

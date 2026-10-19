int one(void) { return 1; }
int (*pick(void))(void) { return one; }
int main() { printf("rfp %d\n", pick()()); return 0; }

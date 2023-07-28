#include "stdlib/lambda.c"

int testlambda_plus1 (int a) {
	stacktrace();
	return a + 1;
}

int testlambda_plus2 (int a, int b) {
	stacktrace();
	return a + b;
}

int main (int argc, char **argv) {
	int *gc, i, *l1, *l2, *l3;

	if ((i = gc_init())) return i;
	if ((i = lambda_init())) return i;
	gc = gc_enable_autocollect();

	l1 = new_lambda1((int*)&testlambda_plus1, 1);
	i  = (int)invoke1((int*)l1[C4Lambda_Invoke], (int)l1);
	printf("Should be %d: %d\n", 1 + 1, i);

	l2 = new_lambda2((int*)&testlambda_plus2, 2, 3);
	i  = (int)invoke1((int*)l2[C4Lambda_Invoke], (int)l2);
	printf("Should be %d: %d\n", 2 + 3, i);

	l3 = new_lambda3((int*)&testlambda_plus2, 2, 3, 4);
	l3 = new_lambda4((int*)&testlambda_plus2, 2, 3, 4, 5);
	l3 = new_lambda5((int*)&testlambda_plus2, 2, 3, 4, 5, 6);

	gc_collect(gc - gc_ptr);
	gc_cleanup();
	return 0;
}

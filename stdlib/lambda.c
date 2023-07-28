// C4 Multiload Class: Lambda
//
// A class that stores some custom data, like variable capture, and can be called for a result.
// Overcomes the lack of lambda callbacks and capture in C.
//

#ifndef _C4STDLIB_LAMBDA
#define _C4STDLIB_LAMBDA 1

#include "classes.c"

enum { _lambda_data, _lambda_words, _lambda_callback, C4Lambda_Invoke, _lambda__sz };

int *lambda_fx_Invoke;

// Generic constructor
void lambda_construct3 (int *self, int *callback, int *words, int length) {
	int *target;
	if(!(self[_lambda_data] = (int)malloc(sizeof(int) * length))) {
		// TODO: throw
		printf("lambda_construct3: malloc fail\n");
		exit(-1);
	}
    self[_lambda_words] = length;
	self[_lambda_callback] = (int)callback;
	// Copy words
	target = (int*)self[_lambda_data];
	while(length--)
		*target++ = *words++;
}

void lambda_destruct (int *self) {
	free((int*)self[_lambda_data]);
}

int lambda_invoke0 (int *self) { return invoke0((int*)self[_lambda_callback]); }
int lambda_invoke1 (int *self) {
	int *data;
	data = (int*)self[_lambda_data];
	return (int)invoke1((int*)self[_lambda_callback], *data);
}
int lambda_invoke2 (int *self) {
	int *data;
	data = (int*)self[_lambda_data];
	return (int)invoke2((int*)self[_lambda_callback], *data, *data + 1);
}
int lambda_invoke3 (int *self) {
	int *data;
	data = (int*)self[_lambda_data];
	return (int)invoke3((int*)self[_lambda_callback], *data, *data + 1, *data + 2);
}
int lambda_invoke4 (int *self) {
	int *data;
	data = (int*)self[_lambda_data];
	return (int)invoke4((int*)self[_lambda_callback], *data, *data + 1, *data + 2, *data + 3);
}
int lambda_invoke5 (int *self) {
	int *data;
	data = (int*)self[_lambda_data];
	return invoke5((int*)self[_lambda_callback], *data, *data + 1, *data + 2, *data + 3, *data + 4);
}

// Generic
int *lambda_construct_ (int *callback, int *words, int length) {
	int *ptr, *inv;
	ptr = object_construct3(_lambda__sz * sizeof(int), (int*)&lambda_construct3, (int*)&lambda_destruct, (int)callback, (int)words, length);
	//ptr[C4Lambda_Invoke] = (int)lambda_fx_Invoke;
	// Assign Invoke based on length
	if (length == 0) inv = (int*)&lambda_invoke0;
	else if(length == 1) inv = (int*)&lambda_invoke1;
	else if(length == 2) inv = (int*)&lambda_invoke2;
	else if(length == 3) inv = (int*)&lambda_invoke3;
	else if(length == 4) inv = (int*)&lambda_invoke4;
	else if(length == 5) inv = (int*)&lambda_invoke5;

	ptr[C4Lambda_Invoke] = (int)inv;

	return ptr;
}

int *new_lambda1 (int *callback, int a) { return lambda_construct_(callback, &a, 1); }
// TODO: Does accessing parameters using &a + 1 to get b really work on all architectures?
// This method compiles but doesn't work under C4:
//int *new_lambda2 (int *callback, int a, ...) { return lambda_construct_(callback, &a, 2); }
//int *new_lambda3 (int *callback, int a, ...) { return lambda_construct_(callback, &a, 3); }
//int *new_lambda4 (int *callback, int a, ...) { return lambda_construct_(callback, &a, 4); }
//int *new_lambda5 (int *callback, int a, ...) { return lambda_construct_(callback, &a, 5); }
int *new_lambda2 (int *callback, int a, int b) { return lambda_construct_(callback, &a, 2); }
int *new_lambda3 (int *callback, int a, int b, int c) { return lambda_construct_(callback, &a, 3); }
int *new_lambda4 (int *callback, int a, int b, int c, int d) { return lambda_construct_(callback, &a, 4); }
int *new_lambda5 (int *callback, int a, int b, int c, int d, int e) { return lambda_construct_(callback, &a, 5); }

int lambda_impl_Invoke (int *self) {
	int words, *data;
	if((words = self[_lambda_words]) == 0)
		return invoke0((int*)self[_lambda_callback]);

	data = (int*)self[_lambda_data];
	if(words == 1)
		return invoke1((int*)self[_lambda_callback], *data);
	else if(words == 2)
		return invoke2((int*)self[_lambda_callback], *data, *data + 1);
	else if(words == 3)
		return invoke3((int*)self[_lambda_callback], *data, *data + 1, *data + 2);
	else if(words == 4)
		return invoke4((int*)self[_lambda_callback], *data, *data + 1, *data + 2, *data + 3);
	else if(words == 5)
		return invoke5((int*)self[_lambda_callback], *data, *data + 1, *data + 2, *data + 3, *data + 4);

	// TODO: exception handling
	printf("C4Lambda::Invoke(): too many arguments\n");
	exit(-1);
}

int lambda_init () {
	lambda_fx_Invoke = (int*)&lambda_impl_Invoke;
	return 0;
}

#endif

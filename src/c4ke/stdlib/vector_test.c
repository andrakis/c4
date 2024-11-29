#include "stdlib/vector.c"

// test a complex class that has an a and b
enum { testvector_cplx_a, testvector_cplx_b, testvector_cplx_sz };

int main (int argc, char **argv) {
	int i, *v1, *v2, *v3, *s1, *s2, *s3;

	if ((i = gc_init())) return i;
	if ((i = C4Vector_init())) return i;

	v1 = new_C4Vector1(1); s1 = gc_push(v1);
	printf("v1: "); invoke1((int*)v1[C4Vector_Print], (int)v1); printf("\n");
	i = 0;
	while(i < 10) invoke2((int*)v1[C4Vector_PushBack], (int)v1, i++);
	printf("v1: "); invoke1((int*)v1[C4Vector_Print], (int)v1); printf("\n");
	v2 = new_C4VectorVector(v1); gc_push(v2);
	while(i < 40) invoke2((int*)v2[C4Vector_PushBack], (int)v2, i++);
	printf("v2: "); invoke1((int*)v2[C4Vector_Print], (int)v2); printf("\n");

	v3 = new_C4Vector1(testvector_cplx_sz);
	gc_push(v3); // TODO: test

	gc_cleanup();
	return 0;
}

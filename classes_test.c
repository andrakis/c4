// Classes test for C4
// Invocation: ./c4 c4_multiload.c classes.c classes_test.c
//
// Implements the below class class swapping.
// class testclass {
// 		public:
// 		int X, Y;
// 		testclass(int x, int y) {
// 			printf("testclass_construct(%d,%d)\n", x, y);
// 			X = x;
// 			Y = y;
// 		}
//		~testclass() { printf("~testclass(%d,%d)\n", X, Y); }
// 		void Print() { printf("testclass{X=%d,Y=%d}", X, Y); }
// 		testclass *Sub() { return new testclass(X - Y, Y - X); }
// 		testclass *Add() { return new testclass(X + Y, Y + X); }
// 		testclass *Mult() { return new testclass(X * Y, Y * X); }
//      testclass *testclass:FactorialX () {
//        if (X <= 1) return this;
//        Y = 1;
//        testclass *tmp1 = Sub();
//        testclass *tmp2 = tmp1->FactorialX();
//        testclass *tmp3 = tmp2->Mult();
//        (garbage collect push tmp1, tmp2, tmp3)
//        swap(this, tmp3);
//        (garbage collect)
//        return this;
//      }
// 		void swap(testclass *other) {
// 			int tmp;
// 			tmp = X; X = other->X; other->X = tmp;
// 			tmp = Y; Y = other->Y; other->Y = tmp;
// 		}
// }

#define CLASSES_NOMAIN
#include "classes.c"

// class testclass {
//   int X, Y;
//   void Print(int*self);
//   int *Sub(int*self); // returns new testclass with result
//   int *Add(int*self); // as above
//   testclass(int x, int y) ..
//   ~testclass() { printf("~testclass(%d,%d)\n", X, Y); }
// }
enum testclass {
	testclass_pub_X, testclass_pub_Y,
	testclass_fun_Print, testclass_fun_Sub, testclass_fun_Add, testclass_fun_Mult,
	testclass_fun_FactorialX, testclass_fun_FactorialY,
	testclass_fun_swap,
	testclass__sz
};
// These are filled in by testclass_init, used in new_testclass before implementation
int *testclass_fx_Print, *testclass_fx_Sub, *testclass_fx_Add, *testclass_fx_Mult;
int *testclass_fx_FactorialX;
int *testclass_fx_swap;

// Constructor
void testclass_construct3 (int *self, int x, int y) {
	printf("testclass_construct@%p(%d,%d)\n", self, x, y);
	self[testclass_pub_X] = x;
	self[testclass_pub_Y] = y;
}

// Destructor
void testclass_destruct (int *self) {
	printf("~testclass@%p(%d,%d)\n", self, self[testclass_pub_X], self[testclass_pub_Y]);
}

// Main way to construct our testclass
int *new_testclass (int x, int y) {
	int *ptr;
	ptr = object_construct2(testclass__sz * sizeof(int), (int*)&testclass_construct3, (int*)&testclass_destruct, x, y);
	// These are filled in by testclass_init
	ptr[testclass_fun_Print] = (int)testclass_fx_Print;
	ptr[testclass_fun_Sub] = (int)testclass_fx_Sub;
	ptr[testclass_fun_Add] = (int)testclass_fx_Add;
	ptr[testclass_fun_Mult] = (int)testclass_fx_Mult;
	ptr[testclass_fun_FactorialX] = (int)testclass_fx_FactorialX;
	ptr[testclass_fun_swap]= (int)testclass_fx_swap;
	return ptr;
}

// Implementation of member functions. Used in new_testclass, assigned by testclass_init.
void testclass_impl_Print (int *self) { printf("testclass{X=%d,Y=%d}", self[testclass_pub_X], self[testclass_pub_Y]); }
int *testclass_impl_Sub   (int *self) {
	return new_testclass(self[testclass_pub_X] - self[testclass_pub_Y],
	                     self[testclass_pub_Y] - self[testclass_pub_X]);
}
int *testclass_impl_Add   (int *self) {
	return new_testclass(self[testclass_pub_X] + self[testclass_pub_Y],
	                     self[testclass_pub_Y] + self[testclass_pub_X]);
}
int *testclass_impl_Mult (int *self) {
	return new_testclass(self[testclass_pub_X] * self[testclass_pub_Y],
	                     self[testclass_pub_Y] * self[testclass_pub_X]);
}

// A test function that uses recursion, creates multiple testclass's, and 
// cleans them up responsibly using the classes.c garbage collector.
// testclass *testclass:FactorialX () {
//   if (X <= 1) return this;
//   Y = 1;
//   testclass *tmp1 = Sub();
//   gc_push(tmp1);
//   testclass *tmp2 = tmp1->FactorialX();
//   if (tmp2 != tmp1) gc_push(tmp2);
//   testclass *tmp3 = tmp2->Mult();
//   gc_push(tmp3);
//   swap(this, tmp3);
//   (garbage collect)
//   return this;
// }
int *testclass_impl_FactorialX (int *self) {
	int *tmp1, *tmp2, *tmp3, *gc_local;

	//printf("factorialX("); invoke1((int*)self[testclass_fun_Print], (int)self); printf(")\n");
	if (self[testclass_pub_X] <= 1) return self;

	// n * factorial(n - 1)
	self[testclass_pub_Y] = 1;
	tmp1 = (int*)invoke1((int*)self[testclass_fun_Sub], (int)self);
	gc_local = gc_ptr;
	gc_push(tmp1);

	tmp2 = (int*)invoke1((int*)self[testclass_fun_FactorialX], (int)tmp1);
	if (tmp2 != tmp1) gc_push(tmp2);

	// multiply with our X
	tmp2[testclass_pub_Y] = self[testclass_pub_X];
	tmp3 = (int*)invoke1((int*)self[testclass_fun_Mult], (int)tmp2);
	gc_push(tmp3);

	// swap with self
	invoke2((int*)self[testclass_fun_swap], (int)self, (int)tmp3);
	// collect
	gc_collect(gc_local - gc_ptr);
	return self;
}

void testclass_impl_swap (int *self, int *other) {
	swap2(&self[testclass_pub_X], &other[testclass_pub_X]);
	swap2(&self[testclass_pub_Y], &other[testclass_pub_Y]);
}

int testclass_init () {
	testclass_fx_Print = (int*)&testclass_impl_Print;
	testclass_fx_Sub = (int*)&testclass_impl_Sub;
	testclass_fx_Add = (int*)&testclass_impl_Add;
	testclass_fx_Mult = (int*)&testclass_impl_Mult;
	testclass_fx_FactorialX = (int*)&testclass_impl_FactorialX;
	testclass_fx_swap= (int*)&testclass_impl_swap;

	return 0;
}

int main (int argc, char **argv) {
	int i, *t1, *t2, *t3, *s1, *s2, *s3;

	if ((i = gc_init())) return i;
	if ((i = testclass_init())) return i;

	// Test basic construction and printing
	t1 = new_testclass(1, 10); s1 = gc_push(t1);
	printf("t1: "); invoke1((int*)t1[testclass_fun_Print], (int)t1); printf("\n");
	// Test member function Sub() and print
	t2 = (int*)invoke1((int*)t1[testclass_fun_Sub], (int)t1); s2 = gc_push(t2);
	printf("t2: "); invoke1((int*)t2[testclass_fun_Print], (int)t2); printf("\n");
	// Test member function Add() and print
	t3 = (int*)invoke1((int*)t1[testclass_fun_Add], (int)t1); s3 = gc_push(t3);
	printf("t3: "); invoke1((int*)t3[testclass_fun_Print], (int)t3); printf("\n");
	// swap t1 with t3
	invoke2((int*)t2[testclass_fun_swap], (int)t1, (int)t2);
	// Collect t3 - t2, leave t1 on stack
	gc_collect(gc_top - s2);

	// Invoke FactorialX with 10 and print
	t1[testclass_pub_X] = 10;
	invoke1((int*)t1[testclass_fun_FactorialX], (int)t1);
	printf("t1: "); invoke1((int*)t1[testclass_fun_Print], (int)t1); printf("\n");
	// Invoke FactorialX with 15 and print
	t1[testclass_pub_X] = 15;
	invoke1((int*)t1[testclass_fun_FactorialX], (int)t1);
	printf("t1: "); invoke1((int*)t1[testclass_fun_Print], (int)t1); printf("\n");

	// Collect anything remaining on the stack
	printf("Final cleanup\n");
	gc_collect_top();
	gc_cleanup();
	return 0;
}

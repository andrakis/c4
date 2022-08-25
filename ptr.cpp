// Experiments with reimplementing C++ std classes
//
// Provides 3 classes:
//   C4UniquePtr      like std::unique_ptr
//   C4WeakPtr        like std::weak_ptr
//   C4SharedPtr      like std::shared_ptr
//

#include <iostream>
#include <cstdlib>

class C4UniquePtr {
	void *_ptr;
	public:
	C4UniquePtr() : C4UniquePtr(0) {}
	C4UniquePtr(void *ptr) : _ptr(ptr) {
		printf("C4UniquePtr@%p(void* %p)\n", this, ptr);
	}
	C4UniquePtr(C4UniquePtr &ptr) : _ptr(0) {
		printf("C4UniquePtr@%p(C4UniquePtr &%p(%p))\n", this, &ptr, ptr._ptr);
		Swap(ptr);
	}
	~C4UniquePtr() { printf("~C4UniquePtr@%p(%p)\n", this, _ptr); if(_ptr) free(_ptr); }
	void *Get () { return _ptr; }
	void  Swap(C4UniquePtr &ptr) {
		void *tmp;
		tmp = _ptr; _ptr = ptr._ptr; ptr._ptr = tmp;
	}
};

void do_a_test(C4UniquePtr &p, int index) {
	C4UniquePtr b(p);
	if (index < 10)
		do_a_test(b, index + 1);
	printf("I %s have the pointer", b.Get() ? "still" : "no longer");
}

int main (int argc, char **argv) {
	long long *a = (long long*)malloc(sizeof(long long)), *b = (long long*)malloc(sizeof(long long));
	if (!a || !b)
		return 1;
	*a = 123;
	*b = 456;

	C4UniquePtr pa(a), pb(b);
	do_a_test(pa, 0);
	return 0;
}

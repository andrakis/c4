// libc4cxx.c -- what the C that c4cxx writes calls for new, delete and
// pure virtual functions (docs/c4cxx-plan.md section 3.7). C, for c4lc and
// gcc alike; linked after the program's units.
//
// Only malloc, free, printf and exit, which every c4 libc has (and c4m's
// load-c4r.c), so it is declared here rather than included.
void *malloc (unsigned int);
void free (void *);
int printf (const char *, ...);
void exit (int);

// new: never 0 (Watcom printed and stopped too)
void *__c4x_new (unsigned int n)
{
	void *p = malloc (n ? n : 1);
	if (!p) {
		printf ("c4cxx: out of memory (new of %u bytes)\n", n);
		exit (1);
	}
	return p;
}

// new T[n] of a class with a constructor or destructor: each element built
// by CTOR (its default constructor), if any; with COOKIE (a destructor),
// the count in the word before the elements, for delete[]. 8 bytes go
// before them so that they stay as aligned as malloc's block.
void *__c4x_new_vec (unsigned int n, unsigned int size, void *(*ctor) (void *), int cookie)
{
	char *p = __c4x_new (n * size + (cookie ? 8 : 0));
	unsigned int i;
	if (cookie) {
		*(unsigned int *)(p + 4) = n;
		p += 8;
	}
	if (ctor)
		for (i = 0; i < n; i++)
			ctor (p + i * size);
	return p;
}

void __c4x_delete (void *p)
{
	if (p)
		free (p);
}

// delete p of a class with a destructor, not virtual
void __c4x_delete_d (void *p, void (*dtor) (void *))
{
	if (p) {
		dtor (p);
		free (p);
	}
}

// delete p of a class whose destructor is virtual: SLOT of the vtable
// whose pointer is p's first word (_vp is at offset 0 in every class that
// has one, plan section 3.2), then the storage, which starts where p
// points with single inheritance
void __c4x_delete_v (void *p, int slot)
{
	if (p) {
		void (**vt) (void *) = *(void (***) (void *))p;
		vt[slot] (p);
		free (p);
	}
}

// delete[] p of a class with a destructor: the elements last first
void __c4x_delete_vec (void *p, unsigned int size, void (*dtor) (void *))
{
	char *q = p;
	unsigned int n;
	if (!p)
		return;
	n = *(unsigned int *)(q - 4);
	while (n--)
		dtor (q + n * size);
	free (q - 8);
}

// a pure virtual function's slot
void __c4x_pure (void)
{
	printf ("c4cxx: a pure virtual function was called\n");
	exit (1);
}

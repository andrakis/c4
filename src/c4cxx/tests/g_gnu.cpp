// g_gnu.cpp: what glibc's and gcc's headers ask of c4cxx when Blood is
// built for the host from c4cxx's C (the M6 oracle): gcc's builtins that
// take a type (__builtin_va_arg, __builtin_offsetof) and __builtin_va_list,
// extern "C++" inside extern "C", an exception specification (glibc's
// __THROW, throw ()), an asm label (__REDIRECT), wchar_t, and a variable
// declared by extern "C" alone (key.h), which is a declaration and not a
// definition. test-cxx.sh runs a g_*.cpp through g++ and c4cxx with gcc,
// not c4lc, which has none of gcc's builtins.
typedef __builtin_va_list va_list;
#define va_start(v, l) __builtin_va_start (v, l)
#define va_arg(v, t) __builtin_va_arg (v, t)
#define va_end(v) __builtin_va_end (v)
#define offsetof(t, m) __builtin_offsetof (t, m)

extern "C" {
int printf (const char *, ...) throw ();
int puts (const char *__s) __asm__ ("" "puts");
extern "C++" {
inline int twice (int x) { return 2 * x; }
inline int twice (const char *s) { return s[0] * 2; }
}
}

extern "C" int counter;
int counter = 5;

struct Rec {
	char tag;
	int value;
	short pad[3];
	long last;
};

static int sum (int n, ...)
{
	va_list ap;
	int s = 0;
	va_start (ap, n);
	for (int i = 0; i < n; i++)
		s += va_arg (ap, int);
	va_end (ap);
	return s;
}

static long widest (const wchar_t *w)
{
	long m = 0;
	for (; *w; w++)
		if (*w > m)
			m = *w;
	return m;
}

int main ()
{
	static const wchar_t ws[] = { 3, 70000, 9, 0 };
	char buf[offsetof (Rec, pad) + 1];
	printf ("sum %d\n", sum (4, 1, 2, 3, 4));
	printf ("offsets %d %d %d %d\n", (int)offsetof (Rec, tag), (int)offsetof (Rec, value),
	        (int)offsetof (Rec, last), (int)sizeof buf);
	printf ("twice %d %d\n", twice (21), twice ("A"));
	printf ("wide %ld %d\n", widest (ws), (int)sizeof (wchar_t));
	printf ("counter %d\n", counter++);
	puts ("puts by its asm name");
	return counter == 6 ? 0 : 1;
}

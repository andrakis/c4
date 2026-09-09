// u0lite.h -- u0, minus everything that needs a kernel or an opcode.
//
// These are u0's own definitions, copied, and the copying is the point
// rather than an oversight. u0 is the C4KE runtime: pids, signals, the
// kernel task table, and the opcode plumbing to reach them. c4cc has
// no preprocessor and compiles every function in a source it is handed
// whether it is called or not, so handing it u0.h costs a program
// OPCD, INFO and JSRS -- opcodes a C4DOS transient does not have, for a
// kernel it cannot talk to. What is left when you take those away is
// this: some string walking, some character tests and a linear
// congruential generator, none of which needs anything above EXIT.
//
// Use ONE of these, never both:
//
//   c4cc -o prog.c4r include/u0.h     prog.c   # the C4KE / C4IX build
//   c4cc -o prog.c4r include/u0lite.h prog.c   # the C4DOS build
//
// Pick wrong and you now find out politely: a u0 build run under C4DOS
// prints "C4DOS: This application requires C4KE." and returns to the
// prompt. u0.h's constructor checks for DOS before it asks the kernel
// for anything -- see __c4dos_api there.
//
// The names match u0's exactly, so the program's source does not know
// which build it is in. That is what lets mandel, rps and the rest be
// ONE source that runs on every rung, with no #ifdef -- which matters,
// because the compiler that runs on the machine has no # to read.
//
// `opscan` says nothing above EXIT, and test-c4bb-baseops keeps saying
// it. See docs/c4bb-storage.md M16.

int strlen (char *s) { char *t; t = s; while (*t) ++t; return t - s; }
int strcmp (char *s1, char *s2) {
	while (*s1 && (*s1 == *s2)) { ++s1; ++s2; }
	return *s1 - *s2;
}

int  isnum   (char c) { return c >= '0' && c <= '9'; }
int  isspace (char c) { return c <= ' '; }
int  isalpha (char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); }
int  isalnum (char c) { return isnum(c) || isalpha(c); }
char tolower (char c) { return c | ' '; }
char toupper (char c) { return c & '_'; }

// The same generator u0 has, so a seeded run gives the same sequence
// on both builds.
enum { RAND_MIN = 0, RAND_MAX = 32768 };
static int __u0_rand_next = 1;
static int srand (int seed) { __u0_rand_next = seed; }
static int rand () {
	__u0_rand_next = __u0_rand_next * 1103515245 + 12345;
	return (__u0_rand_next / 65536) % RAND_MAX;
}

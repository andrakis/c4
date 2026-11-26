// rt-gxx.cpp: what g++'s reference programs (test-cxx.sh) need from a C++
// library that this host has no 32-bit build of: new, delete and the
// pure virtual handler, on malloc and free
extern "C" void *malloc (unsigned int);
extern "C" void free (void *);
extern "C" int printf (const char *, ...);
extern "C" void exit (int);

void *operator new (unsigned int n) { return malloc (n ? n : 1); }
void *operator new[] (unsigned int n) { return malloc (n ? n : 1); }
void operator delete (void *p) { free (p); }
void operator delete[] (void *p) { free (p); }
extern "C" void __cxa_pure_virtual () { printf ("pure virtual\n"); exit (1); }

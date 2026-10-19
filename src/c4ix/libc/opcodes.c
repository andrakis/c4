// libc: the VM's own allocator and memory opcodes, under names the rest
// of the library can call.
//
// This one file is compiled WITHOUT -fno-builtin, so here malloc, free,
// memcpy, memset and memcmp are the MALC, FREE, MCPY, MSET and MCMP
// opcodes, run at host speed. Everywhere else in the library those names
// are the library's own functions, which call these. (__c4_opcode cannot
// stand in: OPCD leaves the opcode number on top of the stack, where an
// opcode that takes arguments reads its last one.)

char *__libc_malloc(int n) { return (char *)malloc(n); }
void __libc_free(char *p) { free(p); }
char *__libc_memcpy(char *d, char *s, int n) { return (char *)memcpy(d, s, n); }
char *__libc_memset(char *d, int c, int n) { return (char *)memset(d, c, n); }
int __libc_memcmp(char *a, char *b, int n) { return memcmp(a, b, n); }

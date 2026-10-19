// sys.h -- what libc's own sources call underneath: libc4ix's syscall
// functions (src/c4ix/lib/libc4ix.c, linked into libc.c4s). Private to
// the library: c4ix_user.h itself cannot be included beside libc.h, since
// it has its own O_RDWR and friends.

#ifndef __C4IX_LIBC_SYS_H
#define __C4IX_LIBC_SYS_H 1

int  uread(int fd, char *buf, int len);
int  uopen(char *path, int flags);
int  uclose(int fd);
int  uexit(int code);
int  udup(int fd);
int  udup2(int oldfd, int newfd);
int  upipe(int *fds);
int  uchdir(char *path);
int  umkdir(char *path);
int  ugetcwd(char *buf, int len);
int  ureaddir(char *path, int index, char *name);
int  umsleep(int ms);
int  ustat(char *path, int *out);
int  uunlink(char *path);
int  urename(char *path, char *newname);
int  getpid();

// the VM's opcodes, from opcodes.c (compiled without -fno-builtin)
char *__libc_malloc(int n);
void  __libc_free(char *p);
char *__libc_memcpy(char *d, char *s, int n);
char *__libc_memset(char *d, int c, int n);
int   __libc_memcmp(char *a, char *b, int n);

#endif

//
// libc.h -- C4IX's C library: everything in one header, which the
// standard names (stdio.h, string.h, ...) include.
//
// A program built against it is compiled with -fno-builtin, so that
// printf, malloc, open and the rest are this library's functions rather
// than the VM's opcodes, and linked with `c4rlink -l libc.c4s`: the
// library is loaded at run time from /lib/libc.c4s (src/c4ix/loader.c),
// one copy per task, so errno and the stdio buffers are each task's own.
//
// What is here is a working subset of C99 and POSIX, written for this
// machine: stdio with buffered FILEs and a full printf, the string and
// ctype functions, malloc with realloc, strtol, qsort, getenv, getopt,
// opendir, and the file calls over C4IX's syscalls (lseek, fstat, link
// and truncate among them), vfork, execve and waitpid, and printf's %f
// %e %g. What is not (yet): locales, wide characters, setjmp and signal
// handlers. docs/c4ix-design.md lists the gaps.
//
// Floating point is math.h's: the math functions, and strtod, strtof and
// atof too, which C puts in stdlib.h. A unit that says float or double
// gets c4lc's float helpers spliced in, which costs every compile of it
// time (half a second on the host, far more inside C4IX), so this header
// says neither, and a unit that wants them includes math.h.
//
// Two departures a C programmer will notice. A function pointer is an
// int here -- c4lc calls any variable -- so qsort and atexit take one.
// And sizes are int: size_t and ssize_t are int, as every word is.
//

#ifndef __C4IX_LIBC_H
#define __C4IX_LIBC_H 1

#define NULL 0
// a member's offset; an expression, not yet a constant c4lc can fold (M7b)
#define offsetof(T, m) ((int)&((T *)0)->m)
#define EOF (-1)
#define BUFSIZ 1024
#define FILENAME_MAX 256
#define PATH_MAX 256
#define INT_MAX 2147483647
#define INT_MIN (-2147483647 - 1)
#define SHRT_MAX 32767
#define SHRT_MIN (-32768)
#define USHRT_MAX 65535
#define SCHAR_MAX 127
#define SCHAR_MIN (-128)
#define UCHAR_MAX 255
#define CHAR_MAX 127
#define CHAR_MIN (-128)
#define CHAR_BIT 8
#define RAND_MAX 32767
#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

typedef int size_t;
typedef int ssize_t;
typedef int off_t;
typedef int pid_t;
typedef int mode_t;
typedef int time_t;

// ---- errno ----
extern int errno;
enum {
    EPERM = 1, ENOENT = 2, ESRCH = 3, EINTR = 4, EIO = 5, EBADF = 9, ECHILD = 10,
    EAGAIN = 11, ENOMEM = 12, EACCES = 13, EEXIST = 17, ENOTDIR = 20, EISDIR = 21,
    EINVAL = 22, EMFILE = 24, ENOSPC = 28, ESPIPE = 29, EROFS = 30, EPIPE = 32,
    ERANGE = 34, ENOSYS = 38, ENOTEMPTY = 39, ENOEXEC = 8
};
char *strerror(int e);
void perror(char *s);

// ---- fcntl / unistd: C4IX's own flag values ----
enum { O_RDONLY = 0, O_WRONLY = 1, O_RDWR = 2, O_ACCMODE = 3,
       O_CREAT = 256, O_TRUNC = 512, O_NONBLOCK = 2048, O_APPEND = 4096, O_EXCL = 8192 };
enum { STDIN_FILENO = 0, STDOUT_FILENO = 1, STDERR_FILENO = 2 };

int  open(char *path, int flags, ...);
int  creat(char *path, int mode);
int  close(int fd);
int  read(int fd, char *buf, int n);
int  write(int fd, char *buf, int n);     // libc4ix's
int  lseek(int fd, int off, int whence);
int  dup(int fd);
int  dup2(int fd, int to);
int  pipe(int *fds);
int  unlink(char *path);
int  remove(char *path);
int  rmdir(char *path);
int  mkdir(char *path, int mode);
int  rename(char *from, char *to);
int  chdir(char *path);
char *getcwd(char *buf, int n);
int  getpid();
int  isatty(int fd);
int  access(char *path, int mode);
unsigned int sleep(unsigned int s);
int  usleep(int us);
void _exit(int code);
int  link(char *path, char *newpath);
int  truncate(char *path, int len);
int  ftruncate(int fd, int len);

// ---- processes: vfork and exec, as a machine without an MMU has them ----
// vfork's child shares the parent's memory until it calls execve or
// _exit -- never exit, whose atexit and stdio flushing are the parent's
// -- and must not return from the function that called vfork. The
// parent waits until it has.
int  vfork();
int  execve(char *path, char **argv, char **envp);
int  execv(char *path, char **argv);      // with this task's environment
int  waitpid(int pid, int *status, int options);
int  wait(int *status);
enum { WNOHANG = 1, WUNTRACED = 2 };
#define WEXITSTATUS(s) (((s) >> 8) & 255)
#define WTERMSIG(s)    ((s) & 127)
#define WIFEXITED(s)   (WTERMSIG(s) == 0)
#define WIFSIGNALED(s) (WTERMSIG(s) != 0)
#define WIFSTOPPED(s)  0

// stat: what C4IX can say about a file. The first four are where they
// always were. Modes are made up, the VFS having none: 0755 for
// directories and programs, 0644 for files; uid and gid are 0.
struct stat {
    int st_mode;
    int st_size;
    int st_mtime;
    int st_nlink;
    int st_ino;         // the same for two names of one file (link)
    int st_dev;         // 1, the RAM filesystem; 2, a host file; 0, the console or a pipe
    int st_uid;
    int st_gid;
    int st_rdev;
    int st_atime;
    int st_ctime;
    int st_blksize;
    int st_blocks;      // 512-byte blocks
};
enum { S_IFMT = 61440, S_IFDIR = 16384, S_IFREG = 32768, S_IFCHR = 8192, S_IFIFO = 4096,
       S_IXUSR = 64 };
#define S_ISDIR(m)  (((m) & S_IFMT) == S_IFDIR)
#define S_ISREG(m)  (((m) & S_IFMT) == S_IFREG)
#define S_ISCHR(m)  (((m) & S_IFMT) == S_IFCHR)
#define S_ISFIFO(m) (((m) & S_IFMT) == S_IFIFO)
int  stat(char *path, struct stat *st);
int  lstat(char *path, struct stat *st);   // stat: there are no symbolic links
int  fstat(int fd, struct stat *st);

// ---- string ----
int   strlen(char *s);
int   strnlen(char *s, int n);
int   strcmp(char *a, char *b);
int   strncmp(char *a, char *b, int n);
int   strcasecmp(char *a, char *b);
int   strncasecmp(char *a, char *b, int n);
char *strcpy(char *d, char *s);
char *strncpy(char *d, char *s, int n);
char *strcat(char *d, char *s);
char *strncat(char *d, char *s, int n);
char *strchr(char *s, int c);
char *strrchr(char *s, int c);
char *strstr(char *h, char *n);
char *strdup(char *s);
char *strndup(char *s, int n);
int   strspn(char *s, char *accept);
int   strcspn(char *s, char *reject);
char *strpbrk(char *s, char *accept);
char *strtok(char *s, char *delim);
char *strtok_r(char *s, char *delim, char **save);
char *memcpy(char *d, char *s, int n);
char *memmove(char *d, char *s, int n);
char *memset(char *d, int c, int n);
int   memcmp(char *a, char *b, int n);
char *memchr(char *s, int c, int n);

// ---- ctype ----
int isalpha(int c); int isdigit(int c); int isalnum(int c); int isspace(int c);
int isupper(int c); int islower(int c); int isxdigit(int c); int isprint(int c);
int ispunct(int c); int iscntrl(int c); int isgraph(int c); int isblank(int c);
int toupper(int c); int tolower(int c);

// ---- stdlib ----
char *malloc(int n);
char *calloc(int n, int size);
char *realloc(char *p, int n);
void  free(char *p);
void  exit(int code);
int   atexit(int fn);
void  abort();
int   abs(int v);
int   labs(int v);
int   atoi(char *s);
int   atol(char *s);
int   strtol(char *s, char **end, int base);
unsigned int strtoul(char *s, char **end, int base);
void  qsort(char *base, int n, int size, int cmp);
char *bsearch(char *key, char *base, int n, int size, int cmp);
char *getenv(char *name);
int   setenv(char *name, char *value, int overwrite);
int   unsetenv(char *name);
int   system(char *cmd);
int   rand();
void  srand(unsigned int seed);

// ---- stdio ----
typedef struct __c4ix_file {
    int fd;
    int flags;          // __F_* below
    char *buf;
    int pos;            // read: next byte; write: bytes waiting
    int len;            // read: bytes in buf
    int ungot;          // -1, or the byte ungetc gave back
} FILE;
enum { __F_READ = 1, __F_WRITE = 2, __F_EOF = 4, __F_ERR = 8, __F_LINE = 16, __F_UNBUF = 32, __F_OWNBUF = 64 };
extern FILE *stdin;
extern FILE *stdout;
extern FILE *stderr;

FILE *fopen(char *path, char *mode);
FILE *fdopen(int fd, char *mode);
int   fclose(FILE *f);
int   fflush(FILE *f);
int   fgetc(FILE *f);
int   getc(FILE *f);
int   getchar();
int   ungetc(int c, FILE *f);
char *fgets(char *s, int n, FILE *f);
int   fputc(int c, FILE *f);
int   putc(int c, FILE *f);
int   putchar(int c);
int   fputs(char *s, FILE *f);
int   puts(char *s);
int   fread(char *p, int size, int n, FILE *f);
int   fwrite(char *p, int size, int n, FILE *f);
int   feof(FILE *f);
int   ferror(FILE *f);
void  clearerr(FILE *f);
int   fileno(FILE *f);
int   setvbuf(FILE *f, char *buf, int mode, int size);
enum { _IOFBF = 0, _IOLBF = 1, _IONBF = 2 };
int   getline(char **line, int *cap, FILE *f);
long  ftell(FILE *f);
int   fseek(FILE *f, long off, int whence);
void  rewind(FILE *f);
int   sscanf(char *s, char *fmt, ...);
int   fscanf(FILE *f, char *fmt, ...);
int   scanf(char *fmt, ...);
int   vsscanf(char *s, char *fmt, int *ap);
int   vfscanf(FILE *f, char *fmt, int *ap);
int   vscanf(char *fmt, int *ap);

int printf(char *fmt, ...);
int fprintf(FILE *f, char *fmt, ...);
int sprintf(char *s, char *fmt, ...);
int snprintf(char *s, int n, char *fmt, ...);
int vprintf(char *fmt, int *ap);
int vfprintf(FILE *f, char *fmt, int *ap);
int vsprintf(char *s, char *fmt, int *ap);
int vsnprintf(char *s, int n, char *fmt, int *ap);
int dprintf(int fd, char *fmt, ...);

// ---- getopt ----
extern char *optarg;
extern int optind, opterr, optopt;
int getopt(int argc, char **argv, char *opts);

// ---- dirent ----
struct dirent {
    char d_name[64];
    int d_type;         // DT_DIR or DT_REG
};
enum { DT_UNKNOWN = 0, DT_REG = 8, DT_DIR = 4 };
typedef struct __c4ix_dir {
    struct dirent *ent;     // what readdir returns, refilled each call
    int index;
    char path[256];
} DIR;
DIR *opendir(char *path);
struct dirent *readdir(DIR *d);
int closedir(DIR *d);

// ---- time ----
time_t time(time_t *t);
// the calendar, in UTC: C4IX keeps no time zone, so localtime is gmtime
struct tm {
    int tm_sec, tm_min, tm_hour;        // 0-60, 0-59, 0-23
    int tm_mday, tm_mon, tm_year;       // 1-31, 0-11, years since 1900
    int tm_wday, tm_yday, tm_isdst;     // 0-6 from Sunday, 0-365, 0
};
struct tm *gmtime(time_t *t);
struct tm *localtime(time_t *t);
time_t mktime(struct tm *tm);
int clock();
enum { CLOCKS_PER_SEC = 1000 };

// ---- varargs, as libc4ix does them (c4ix_user.h) ----
#ifndef va_start
#define va_list int *
#define va_arg(AP, TYPE)   (AP = AP + 1, *((TYPE *) (AP - 1)))
#define va_start(AP, LAST) (AP = *(&LAST - 1), va_arg(AP, int))
#define va_end(AP)         (AP = AP - 1, __c4ix_uva_adj(1 + va_arg(AP, int)))
#define va_copy(D, S)      (D = S)
int *__c4cc_make_va(int count);
void __c4ix_uva_adj(int n);
#endif

#define assert(e) ((e) ? 0 : __assert_fail(#e, "", 0))
int __assert_fail(char *e, char *file, int line);

#endif // __C4IX_LIBC_H

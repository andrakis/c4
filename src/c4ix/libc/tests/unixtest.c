// unixtest -- the kernel's POSIX calls, through libc (M7c,
// docs/busybox-port.md §4): lseek; fstat and stat; O_APPEND and O_EXCL;
// link and truncate; waitpid; and vfork with execve and an environment.
// Its output is compared with unixtest.txt (libjs/tests/test-libjs.sh),
// and every line says what it checks.
//
//   unixtest              the test
//   unixtest child ARGS   what it execs: prints ARGS and $GREETING, exits 10 + argc
//   unixtest sleep MS     sleeps, then exits 3
//   unixtest fd N         says whether descriptor N is open
//
// Busybox's NOMMU code is the model for the vfork cases: the parent
// flushes stdio first; the child calls only execve, _exit and the plain
// descriptor calls, and tells the parent through its locals.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/wait.h>

int ukill(int pid, int sig);       // libc4ix's, in libc.c4s; kill() is M7d's
int ucloexec(int fd, int on);      // fcntl(F_SETFD) is M7d's

int fcheck(int fd);

char *self = "/bin/unixtest";
char *env_hello[2];

static void say(char *s) { write(1, s, strlen(s)); }

// a whole small file, as a string
static char *slurp(char *path) {
    static char buf[64];
    int fd, n;
    buf[0] = 0;
    if ((fd = open(path, O_RDONLY)) < 0) return "(none)";
    n = read(fd, buf, 63);
    close(fd);
    buf[n > 0 ? n : 0] = 0;
    return buf;
}

// Deep enough to write well below the frame that called vfork: on the
// parent's stack, this would be over the parent's own state.
static int churn(int n) {
    int a[16], i;
    i = 0;
    while (i < 16) { a[i] = n * 16 + i; ++i; }
    if (n > 0) return churn(n - 1) + a[3] - a[2];
    return 0;
}

static int child_main(int argc, char **argv) {
    int i;
    char *g;
    printf("child:");
    i = 2;
    while (i < argc) { printf(" %s", argv[i]); ++i; }
    g = getenv("GREETING");
    printf(" GREETING=%s\n", g ? g : "(unset)");
    fflush(stdout);
    return 10 + argc;
}

static void test_lseek() {
    char b[8];
    int fd, p[2], r1, r2, r3, r4, bad, e1, pipe_r, e2, size;
    struct stat st;
    fd = open("/tmp/ut", O_RDWR | O_CREAT | O_TRUNC, 420);
    write(fd, "0123456789", 10);
    r1 = lseek(fd, 3, SEEK_SET);
    read(fd, b, 2); b[2] = 0;
    printf("lseek: set %d read %s", r1, b);
    r2 = lseek(fd, -2, SEEK_END);
    read(fd, b, 2); b[2] = 0;
    r3 = lseek(fd, 0, SEEK_CUR);
    printf(" end %d read %s cur %d", r2, b, r3);
    // past the end: the gap reads as zeros
    r4 = lseek(fd, 4, SEEK_END);
    write(fd, "X", 1);
    fstat(fd, &st);
    size = st.st_size;
    lseek(fd, 10, SEEK_SET);
    read(fd, b, 5);
    printf(" past %d size %d gap %d%d%d%d%c\n", r4, size, b[0], b[1], b[2], b[3], b[4]);
    bad = lseek(fd, -1, SEEK_SET); e1 = errno;
    pipe(p);
    pipe_r = lseek(p[0], 0, SEEK_SET); e2 = errno;
    printf("lseek: before the start %d %s, a pipe %d %s\n", bad, e1 == EINVAL ? "EINVAL" : "?", pipe_r, e2 == ESPIPE ? "ESPIPE" : "?");
    close(p[0]); close(p[1]);
    close(fd);
}

static void test_stat() {
    struct stat a, b, d, pr;
    int fd, m1, p[2];
    fd = open("/tmp/ut", O_RDWR);
    fstat(fd, &a);
    stat("/tmp/ut", &b);
    stat("/tmp", &d);
    stat(self, &pr);
    printf("stat: size %d mode %o nlink %d same file %d, /tmp %o dir %d, program %o\n", a.st_size, a.st_mode, a.st_nlink,
           a.st_ino == b.st_ino && a.st_dev == b.st_dev && a.st_ino != d.st_ino, d.st_mode, S_ISDIR(d.st_mode) != 0, pr.st_mode);
    m1 = a.st_mtime;
    write(fd, "y", 1);
    fstat(fd, &a);
    pipe(p);
    fstat(p[1], &b);
    printf("stat: a write moves mtime %d, a pipe is a fifo %d, uid %d gid %d, nothing there %d %s\n", a.st_mtime > m1,
           S_ISFIFO(b.st_mode) != 0, a.st_uid, a.st_gid, stat("/tmp/none", &b), errno == ENOENT ? "ENOENT" : "?");
    close(p[0]); close(p[1]);
    close(fd);
}

static void test_append() {
    int fd, f1, f2, e1, e2, x1, x2;
    fd = open("/tmp/ut2", O_WRONLY | O_CREAT | O_TRUNC, 420);
    write(fd, "ab", 2);
    close(fd);
    fd = open("/tmp/ut2", O_WRONLY | O_APPEND);
    lseek(fd, 0, SEEK_SET);           // O_APPEND writes at the end whatever the position
    write(fd, "cd", 2);
    close(fd);
    f1 = open("/tmp/ut2", O_WRONLY | O_APPEND);
    f2 = open("/tmp/ut2", O_WRONLY | O_APPEND);
    write(f1, "1", 1);
    write(f2, "2", 1);                // after f1's byte, not over it
    close(f1); close(f2);
    printf("append: %s\n", slurp("/tmp/ut2"));
    x1 = open("/tmp/ut2", O_WRONLY | O_CREAT | O_EXCL, 420); e1 = errno;
    unlink("/tmp/ut3");
    x2 = open("/tmp/ut3", O_WRONLY | O_CREAT | O_EXCL, 420);
    close(x2);
    fd = open("/tmp/ut3", O_WRONLY | O_CREAT | O_EXCL, 420); e2 = errno;
    printf("excl: existing %d %s, new %d, again %d %s\n", x1, e1 == EEXIST ? "EEXIST" : "?", x2 >= 0, fd, e2 == EEXIST ? "EEXIST" : "?");
}

static void test_link() {
    struct stat a, b;
    DIR *d;
    struct dirent *e;
    int r, e1, e2, fd, seen;
    r = link("/tmp/ut2", "/tmp/ut2b");
    stat("/tmp/ut2", &a);
    stat("/tmp/ut2b", &b);
    printf("link: %d same %d nlink %d %d", r, a.st_ino == b.st_ino, a.st_nlink, b.st_nlink);
    r = link("/tmp/ut2", "/tmp/ut3"); e1 = errno;
    printf(", onto a file %d %s", r, e1 == EEXIST ? "EEXIST" : "?");
    r = link("/tmp", "/tmp/dirlink"); e2 = errno;
    printf(", a directory %d %s\n", r, e2 == EPERM ? "EPERM" : "?");
    // written through one name, read through the other
    fd = open("/tmp/ut2b", O_WRONLY | O_APPEND);
    write(fd, "L", 1);
    close(fd);
    printf("link: through the other name %s", slurp("/tmp/ut2"));
    // the first name goes, and the file stays under the second
    unlink("/tmp/ut2");
    stat("/tmp/ut2b", &b);
    printf(", first name gone %d, kept %s nlink %d\n", stat("/tmp/ut2", &a), slurp("/tmp/ut2b"), b.st_nlink);
    // a link in another directory; the file's own name goes again, so
    // the file moves there, and a rename moves only a name
    mkdir("/tmp/ud", 493);
    link("/tmp/ut2b", "/tmp/ud/x");
    unlink("/tmp/ut2b");
    rename("/tmp/ud/x", "/tmp/ud/y");
    d = opendir("/tmp/ud");
    seen = 0;
    while ((e = readdir(d))) { if (!strcmp(e->d_name, "y") && e->d_type == DT_REG) seen = 1; else seen = -1; }
    closedir(d);
    stat("/tmp/ud/y", &b);
    printf("link: moved to /tmp/ud/y %d %s nlink %d", seen, slurp("/tmp/ud/y"), b.st_nlink);
    printf(", last name gone %d %d\n", unlink("/tmp/ud/y"), stat("/tmp/ud/y", &b));
    rmdir("/tmp/ud");
}

static void test_truncate() {
    struct stat st;
    char b[16];
    int fd, ro, p[2], s1, s2, s3, r1, r2;
    fd = open("/tmp/ut", O_RDWR);
    ftruncate(fd, 4);
    fstat(fd, &st); s1 = st.st_size;
    ftruncate(fd, 8);
    fstat(fd, &st); s2 = st.st_size;
    lseek(fd, 0, SEEK_SET);
    read(fd, b, 8);
    printf("truncate: %d then %d: %c%c%c%c %d%d%d%d", s1, s2, b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7]);
    close(fd);
    truncate("/tmp/ut", 2);
    stat("/tmp/ut", &st); s3 = st.st_size;
    ro = open("/tmp/ut", O_RDONLY);
    r1 = ftruncate(ro, 0);
    close(ro);
    pipe(p);
    r2 = ftruncate(p[1], 0);
    close(p[0]); close(p[1]);
    printf(", by name %d, read-only %d, a pipe %d\n", s3, r1, r2);
}

// waitpid, on children made by vfork and execve
static void test_waitpid() {
    char *av[4];
    int pid, pid2, r, st, e;
    av[0] = "unixtest"; av[1] = "sleep"; av[2] = "300"; av[3] = 0;
    fflush(stdout);
    if (!(pid = vfork())) { execve(self, av, env_hello); _exit(127); }
    r = waitpid(pid, &st, WNOHANG);
    printf("waitpid: still running %d", r);
    r = waitpid(-1, &st, 0);
    printf(", then any child %d exited %d status %d\n", r == pid, WIFEXITED(st) != 0, WEXITSTATUS(st));
    r = waitpid(-1, &st, 0); e = errno;
    printf("waitpid: no children %d %s, not a child %d\n", r, e == ECHILD ? "ECHILD" : "?", waitpid(1, &st, WNOHANG));
    // killed: the signal is in the status
    av[2] = "100000";
    fflush(stdout);
    if (!(pid = vfork())) { execve(self, av, env_hello); _exit(127); }
    av[2] = "10";
    if (!(pid2 = vfork())) { execve(self, av, env_hello); _exit(127); }
    ukill(pid, 15);
    r = waitpid(pid, &st, 0);
    printf("waitpid: killed %d signalled %d signal %d", r == pid, WIFSIGNALED(st) != 0, WTERMSIG(st));
    r = wait(&st);
    printf(", wait %d status %d\n", r == pid2, WEXITSTATUS(st));
}

static void test_vfork() {
    char *av[5];
    char kb[8];
    volatile int failed;
    int pid, r, st, fd, keep, sum;

    // exec with an environment, and the child's exit status
    av[0] = "unixtest"; av[1] = "child"; av[2] = "a"; av[3] = "b"; av[4] = 0;
    fflush(stdout);
    if (!(pid = vfork())) { execve(self, av, env_hello); _exit(127); }
    waitpid(pid, &st, 0);
    printf("vfork: exec'd child exited %d\n", WEXITSTATUS(st));

    // The parent's state below the vfork call survives a child that
    // calls deep: 1000 was pushed before vfork was called
    fflush(stdout);
    r = 1000 + vfork();
    if (r == 1000) { sum = churn(40); _exit(sum == 40 ? 5 : 6); }
    // r - 1000 is the child's id only if the 1000 survived
    printf("vfork: the parent's pending sum survived %d", waitpid(r - 1000, &st, 0) == r - 1000);
    printf(", the child churned and exited %d\n", WEXITSTATUS(st));

    // a child that fails to exec says why through the parent's local,
    // as Busybox's spawn() does, and _exits
    failed = 0;
    av[0] = "nothing";
    fflush(stdout);
    if (!(pid = vfork())) {
        execve("/bin/no-such-program", av, env_hello);
        failed = errno;
        _exit(111);
    }
    waitpid(pid, &st, 0);
    printf("vfork: exec failed %s, status %d\n", failed == ENOENT ? "ENOENT" : "?", WEXITSTATUS(st));

    // The child redirects its stdout and writes before it execs, and the
    // program it execs inherits the redirection. The parent's fd 1 is
    // its own still. A close-on-exec descriptor is the child's until it
    // execs, and not the new program's.
    keep = open("/tmp/ut", O_RDONLY);
    ucloexec(keep, 1);
    av[0] = "unixtest"; av[1] = "child"; av[2] = "redirected"; av[3] = 0;
    fflush(stdout);
    if (!(pid = vfork())) {
        fd = open("/tmp/vout", O_WRONLY | O_CREAT | O_TRUNC, 420);
        dup2(fd, 1);
        close(fd);
        say("pre-exec ");
        if (fcheck(keep)) say("has-keep ");
        execve(self, av, 0);
        _exit(127);
    }
    waitpid(pid, &st, 0);
    printf("vfork: the parent's stdout, and the child's: %s", slurp("/tmp/vout"));
    sprintf(kb, "%d", keep);
    av[1] = "fd"; av[2] = kb; av[3] = 0;
    fflush(stdout);
    if (!(pid = vfork())) { execve(self, av, 0); _exit(127); }
    waitpid(pid, &st, 0);
    close(keep);
    unlink("/tmp/vout");
}

// is descriptor FD open?
int fcheck(int fd) {
    struct stat st;
    return fstat(fd, &st) == 0;
}

int main(int argc, char **argv) {
    int ok;
    if (argc >= 2 && !strcmp(argv[1], "child")) return child_main(argc, argv);
    if (argc >= 3 && !strcmp(argv[1], "sleep")) { usleep(atoi(argv[2]) * 1000); return 3; }
    if (argc >= 3 && !strcmp(argv[1], "fd")) {
        printf("vfork: after exec, the close-on-exec fd %s\n", fcheck(atoi(argv[2])) ? "is open" : "is closed");
        return 0;
    }
    env_hello[0] = "GREETING=hello";
    env_hello[1] = 0;
    test_lseek();
    test_stat();
    test_append();
    test_link();
    test_truncate();
    test_waitpid();
    test_vfork();
    // exec that fails in a task that is no vfork child changes nothing
    ok = execve("/bin/no-such-program", argv, 0);
    printf("execve: missing %d %s, still here\n", ok, errno == ENOENT ? "ENOENT" : "?");
    unlink("/tmp/ut");
    unlink("/tmp/ut3");
    printf("unixtest: done\n");
    return 0;
}

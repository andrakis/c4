// C4IX X2 userland program: linked against libc4ix.c4l and run
// behind the protected-mode boundary on c4m. It never executes a
// host opcode -- every line below is a syscall or a library call
// over write(fd) -- so the kernel sees, and could redirect, all of
// it. Compare src/c4ix/user/hello.c, which calls printf directly
// and gets emulated onto the same fd layer by the PM trap.

#include "c4ix_user.h"

int main(int argc, char **argv) {
    int pid, n;

    uprintf("uhello: userland speaking, pid %d argc %d\n", getpid(), argc);

    n = write(STDOUT, "uhello: raw write to fd 1\n", 26);
    ufprintf(STDERR, "uhello: fd 2 works too (wrote %d bytes)\n", n);

    uyield();
    uprintf("uhello: still here after yielding\n");

    pid = getpid();
    uprintf("uhello: exiting with %d\n", 7);
    uexit(7);
    uprintf("uhello: UNREACHABLE (pid %d)\n", pid);
    return 0;
}

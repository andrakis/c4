// C4IX X1 spawn-test program: compiled whole-program by c4lc -O into
// c4ix-hello.c4r and loaded by the kernel from the host filesystem.
// At X1 tasks run unprotected, so host opcodes (printf here) work
// directly; X2 moves userland onto syscalls.

int greet(int n) {
    return n * 2;
}

int main(int argc, char **argv) {
    printf("hello: running as a c4ix task\n");
    printf("hello: argc %d doubled six %d\n", argc, greet(3));
    return 42;
}

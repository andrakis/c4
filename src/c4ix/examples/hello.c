// hello.c -- the smallest program to compile inside C4IX.
//
//   c4cc -o /tmp/hello.c4r /usr/src/c4ix/examples/hello.c
//   /tmp/hello.c4r
//
// printf here is the machine's own, which C4IX sends to this task's
// standard output, so it lands wherever the shell pointed it.

int main(int argc, char **argv) {
    int i;
    printf("hello from a program compiled inside C4IX\n");
    i = 0;
    while (i < argc) { printf("  argv[%d] = %s\n", i, argv[i]); i = i + 1; }
    return 0;
}

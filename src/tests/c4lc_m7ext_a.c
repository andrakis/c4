// M7b: data words naming another unit's functions -- patch type -5 in
// the object, which c4rlink resolves when it links the unit defining them.
// An applet table like Busybox's applet_main[], a struct table, and a
// function pointer, all over mains in c4lc_m7ext_b.c.
int cat_main(int argc, char **argv);
int echo_main(int argc, char **argv);
struct applet { char *name; int (*main)(int, char **); int flags; };
struct applet applets[] = { { "cat", cat_main, 1 }, { "echo", echo_main, 2 } };
int (*mains[])(int, char **) = { cat_main, echo_main };
int (*first)(int, char **) = &cat_main;
int (*second)(int, char **) = echo_main;
// and another unit's globals: patch type -5 over a data symbol
extern int shared_n;
extern char shared_s[];
int *pshared = &shared_n;
char *sshared = shared_s;
// an address plus an offset (docs/ports-feasibility.md 8, step 4): the
// offset rides in the data word, and c4rlink adds it
char *sfar[] = { &shared_s[1], (char *)&shared_s[2] };
int main(void) {
    int i;
    for (i = 0; i < 2; i++)
        printf("%s %d %d %d\n", applets[i].name, applets[i].main(i, 0), mains[i](i + 5, 0), applets[i].flags);
    printf("first %d %d\n", first(9, 0), second(1, 0));
    printf("shared %d %s\n", *pshared, sshared);
    printf("offset %s %s\n", sfar[0], sfar[1]);
    return 0;
}

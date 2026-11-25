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
// a member or an element of another unit's global, at an offset known at compile
// time: it rides in the code patch's value, and c4rlink adds it. And the same of
// this unit's, and of a local.
struct pt { int x; int y; char c; char d; int z; };
extern struct pt gone;
extern struct pt gpts[3];
struct pt mine[2];
void t_offsets(void) {
    struct pt lp;
    struct pt la[2];
    int *ip;
    char *cp;
    gone.x = 11; gone.y = 12; gone.c = 13; gone.d = 14; gone.z = 15;
    gpts[0].y = 21; gpts[1].y = 22; gpts[2].y = 23; gpts[2].d = 24; gpts[2].z = 25;
    printf("ext %d %d %d %d %d\n", gone.x, gone.y, gone.c, gone.d, gone.z);
    printf("exta %d %d %d %d %d\n", gpts[0].y, gpts[1].y, gpts[2].y, gpts[2].d, gpts[2].z);
    ip = &gone.y; cp = &gone.d; printf("extp %d %d\n", *ip, *cp);
    ip = &gpts[2].z; *ip = 26; printf("extq %d\n", gpts[2].z);
    cp = &shared_s[1]; printf("exts %c %c\n", *cp, shared_s[2]);
    gpts[1].y++; ++gpts[1].y; printf("exti %d\n", gpts[1].y--); printf("extd %d\n", gpts[1].y);
    mine[1].y = 31; mine[1].d = 32; printf("mine %d %d\n", mine[1].y, mine[1].d);
    lp.x = 41; lp.y = 42; lp.c = 43; lp.d = 44; lp.z = 45;
    la[1].y = 46; la[1].d = 47; la[0].z = 48;
    printf("loc %d %d %d %d %d\n", lp.x, lp.y, lp.c, lp.d, lp.z);
    printf("loca %d %d %d\n", la[1].y, la[1].d, la[0].z);
    ip = &lp.y; cp = &lp.d; printf("locp %d %d\n", *ip, *cp);
}
// a local's value through STL: what the assignment's own value is, and ++ and --
void t_locals(void) {
    int a, b, c;
    int v[4];
    int *p;
    unsigned char uc;
    char ch;
    a = 5;
    b = (a = 7) + 1; printf("stl %d %d\n", a, b);
    c = a = b = 3; printf("stl-chain %d %d %d\n", a, b, c);
    a = a + 1; a += 2; printf("stl-self %d\n", a);
    b = a++; printf("stl-post %d %d\n", a, b);
    b = a--; printf("stl-postd %d %d\n", a, b);
    b = ++a; printf("stl-pre %d %d\n", a, b);
    b = --a; printf("stl-pred %d %d\n", a, b);
    a++; a--; ++a; --a; printf("stl-stmt %d\n", a);
    v[0] = 1; v[1] = 2; v[2] = 3; v[3] = 4;
    p = v; b = *p++; c = *p++; printf("stl-ptr %d %d %d\n", b, c, *p);
    p = v + 3; b = *p--; printf("stl-ptrd %d %d\n", b, *p);
    p = v; *p++ = 9; *p++ = 8; printf("stl-pstore %d %d %d\n", v[0], v[1], (int)(p - v));
    for (a = 0, b = 10; a < 3; a++, b--) c = a + b;
    printf("stl-for %d %d %d\n", a, b, c);
    uc = 200; ch = 100; uc++; ch++; printf("stl-narrow %d %d\n", uc, ch);
    a = (uc = 255) + 1; printf("stl-uc %d\n", a);
    a = (ch = 200); printf("stl-ch %d\n", a);
    v[1] = (unsigned char)300; printf("mask %d\n", v[1]);
    uc = (unsigned char)a; ch = (unsigned char)a; printf("mask-store %d %d\n", uc, ch);
}
int main(void) {
    int i;
    t_offsets();
    t_locals();
    for (i = 0; i < 2; i++)
        printf("%s %d %d %d\n", applets[i].name, applets[i].main(i, 0), mains[i](i + 5, 0), applets[i].flags);
    printf("first %d %d\n", first(9, 0), second(1, 0));
    printf("shared %d %s\n", *pshared, sshared);
    printf("offset %s %s\n", sfar[0], sfar[1]);
    return 0;
}

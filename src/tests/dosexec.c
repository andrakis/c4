// dosexec.c -- C4DOS's API v4 from a program: exec, load/start/unload,
// catch, unlink, the console slots and the sink. The pin for re-entrant
// RUN (docs/c4dos-design.md, docs/grin-plan.md G0).
//
// Strict c4 and include/c4dos.h, so one source runs everywhere C4DOS
// does -- under unmodified c4, native c4m, c4bb and libjs -- and builds
// with c4cc and with c4lc:
//   ./c4cc -o dosexec.c4r include/c4dos.h src/tests/dosexec.c
// Usage:  RUN dosexec.c4r deep N | resident | catch | file | unlink |
//                         read | sink | leak KB | gen

int streq (char *a, char *b) {
  while (*a && *a == *b) { ++a; ++b; }
  return *a == *b;
}

// three calls down, then out: nothing after dos_exit runs
int down3 (int s) { dos_exit(s); printf("dosexec: dos_exit returned!\n"); return 0 - 1; }
int down2 (int s) { return down3(s) + 100; }
int down1 (int s) { return down2(s) + 100; }

// "dosexec deep N": level N runs level N-1, level 0 exits with 3 from
// three calls down. Each level adds 10 to what its child gave it, and
// level 2 leaves through dos_exit rather than returning, so a frame left
// both ways sits in the middle.
int deep (int n) {
  char *cmd;
  int r;
  if (n <= 0) {
    printf("dosexec: level 0 exits with 3 from three calls down\n");
    down1(3);
    return 99;
  }
  cmd = malloc(32);
  r = 0;                             // no memcpy on plain c4 (MCPY is c4m's)
  while ((cmd[r] = "dosexec deep 0"[r])) ++r;
  cmd[13] = '0' + n - 1;
  r = dos_exec(cmd);
  free(cmd);
  printf("dosexec: level %d: child returned %d\n", n, r);
  if (n == 2) down1(r + 10);
  return r + 10;
}

// "dosexec resident": dosres.c4r loaded once, started twice -- the
// second start sees the first one's globals -- then unloaded, after
// which its handle is dead.
int resident () {
  int h, r;
  char **av;
  av = (char **)malloc(3 * sizeof(char *));
  av[0] = "dosres";
  av[1] = 0;
  av[2] = 0;
  h = dos_load("dosres.c4r");
  printf("dosexec: load gave %s\n", h ? "a handle" : "0");
  if (!h) return 1;
  r = dos_start(h, 1, av);
  printf("dosexec: first start returned %d\n", r);
  av[1] = "x";
  r = dos_start(h, 2, av);
  printf("dosexec: second start (it exits) returned %d\n", r);
  printf("dosexec: unload returned %d\n", dos_unload(h));
  printf("dosexec: unload again returned %d\n", dos_unload(h));
  printf("dosexec: start after unload returned %d\n", dos_start(h, 1, av));
  free((char *)av);
  return 0;
}

int quitter (int a) { return down1(a * 6); }
int plain (int a) { return a + 1; }

int catcher () {
  int r, ex;
  ex = 9;
  r = dos_catch((int *)&quitter, 7, &ex);
  printf("dosexec: catch of an exit gave %d, exited %d\n", r, ex);
  ex = 9;
  r = dos_catch((int *)&plain, 5, &ex);
  printf("dosexec: catch of a return gave %d, exited %d\n", r, ex);
  return 0;
}

// "dosexec write" makes kid.txt, and leaves memory behind for HEAP.SYS
// to take back; "dosexec file" runs it and reads the file afterwards.
int writer () {
  char *junk;
  if (dos_put("kid.txt", "made by a child\n", 16) != 16) {
    printf("dosexec: could not write kid.txt\n");
    return 1;
  }
  junk = malloc(100000);
  junk[0] = 1;
  printf("dosexec: child wrote kid.txt\n");
  return 0;
}

int file () {
  char *buf;
  int n;
  printf("dosexec: the child returned %d\n", dos_exec("dosexec write"));
  buf = malloc(4200);                // dos_slurp reads 4 KB at a go
  n = dos_slurp("kid.txt", buf, 200);
  if (n < 0) n = 0;
  buf[n] = 0;
  printf("dosexec: kid.txt after the child: %d bytes: %s", n, buf);
  free(buf);
  return 0;
}

int unlinker () {
  dos_put("gone.txt", "doomed\n", 7);
  printf("dosexec: unlink gone.txt: %d\n", dos_unlink("gone.txt"));
  printf("dosexec: open after unlink: %d\n", dos_fopen("gone.txt"));
  printf("dosexec: unlink again: %d\n", dos_unlink("gone.txt"));
  return 0;
}

int reader () {
  char *buf;
  int n;
  buf = malloc(128);
  n = dos_conread(buf, 100);
  if (n < 0) n = 0;
  buf[n] = 0;
  printf("dosexec: conread %d bytes: %s", n, buf);
  dos_conwrite("dosexec: conwrite works\n", 24);
  free(buf);
  return 0;
}

// the sink: console output to a function of ours
char *g_cap;
int g_capn;
int g_capctx;
int capture (int ctx, char *buf, int n) {
  int i;
  g_capctx = ctx;
  i = 0;
  while (i < n && g_capn < 120) { g_cap[g_capn] = buf[i]; ++g_capn; ++i; }
  return n;
}
int sinker () {
  g_cap = malloc(128);
  g_capn = 0;
  dos_sink((int *)&capture, 0, 77);
  dos_conwrite("into the sink", 13);
  dos_sink(0, 0, 0);
  g_cap[g_capn] = 0;
  printf("dosexec: the sink got '%s', ctx %d\n", g_cap, g_capctx);
  dos_conwrite("dosexec: and the console is back\n", 33);
  return 0;
}

// "dosexec leak KB": allocate and never free
int leak (int kb) {
  int i;
  char *p;
  i = 0;
  while (i < kb) { p = malloc(1024); p[0] = 1; ++i; }
  printf("dosexec: left %d KB behind\n", kb);
  return 0;
}

// "dosexec gen": slots 26 and 27. With HEAP.SYS a loaded image has a
// generation of its own, newer than ours, and a megabyte allocated in
// it goes when it is unloaded; without, gen is 0 and setgen -1.
int gens () {
  int mine, h, hg, old;
  char *p;
  mine = dos_gen(0);
  if (!mine) {
    printf("dosexec: no generations (setgen %d)\n", dos_setgen(5));
    return 0;
  }
  if (!(h = dos_load("dosres.c4r"))) { printf("dosexec: gen: load failed\n"); return 1; }
  hg = dos_gen(h);
  old = dos_setgen(hg);
  p = malloc(1048576);
  p[0] = 1;
  printf("dosexec: setgen gave back %s, and %s\n", old == mine ? "mine" : "another",
         dos_setgen(old) == hg ? "the image's came back" : "something else came back");
  printf("dosexec: the image's generation is %s\n", hg > mine ? "newer than mine" : "not newer");
  // the megabyte is the image's, so it goes with the image, not with us
  printf("dosexec: before the unload\n");
  dos_exec("MEM");
  dos_unload(h);
  printf("dosexec: after the unload\n");
  dos_exec("MEM");
  return 0;
}

int num (char *s) {
  int n;
  n = 0;
  while (*s >= '0' && *s <= '9') { n = n * 10 + *s - '0'; ++s; }
  return n;
}

int main (int argc, char **argv) {
  if (!dos_present() || dos_version() < C4DOS_API_V4) {
    printf("dosexec: needs C4DOS API v4 (have %d)\n", dos_version());
    return 1;
  }
  if (argc < 2) { printf("usage: dosexec deep N|resident|catch|file|unlink|read|sink|leak KB|gen\n"); return 1; }
  if (streq(argv[1], "deep")) return deep(argc > 2 ? num(argv[2]) : 3);
  if (streq(argv[1], "resident")) return resident();
  if (streq(argv[1], "catch")) return catcher();
  if (streq(argv[1], "write")) return writer();
  if (streq(argv[1], "file")) return file();
  if (streq(argv[1], "unlink")) return unlinker();
  if (streq(argv[1], "read")) return reader();
  if (streq(argv[1], "sink")) return sinker();
  if (streq(argv[1], "leak")) return leak(argc > 2 ? num(argv[2]) : 1024);
  if (streq(argv[1], "gen")) return gens();
  printf("dosexec: what is %s?\n", argv[1]);
  return 1;
}

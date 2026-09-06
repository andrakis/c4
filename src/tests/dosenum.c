// dosenum.c -- ask C4DOS what it is holding, through the v2 API.
//
// The v1 API could open a file BY NAME, which is all a tool needs and
// not enough for a loader: something taking the machine over wants the
// whole RAM disk without being told what is on it, the way a boot
// loader is handed an initrd. dosenum is the smallest program that
// exercises that -- and the pin that keeps slots 9-14 honest, since
// the kernel extension that seeds C4KE reads exactly these.
//
// Build:  ./c4cc -o dosenum.c4r include/c4dos.h src/tests/dosenum.c
// Usage:  RUN dosenum.c4r [-t] [-r]
//           -t  ask DOS to give its read scratch back (trim)
//           -r  ask DOS to drop the RAM disk contents (release)

int main (int argc, char **argv) {
  int n, i, sz, do_trim, do_rel;
  char *nm, *d, *a;

  do_trim = 0;
  do_rel = 0;
  i = 1;
  while (i < argc) {
    a = argv[i];
    if (a[0] == '-' && a[1] == 't') do_trim = 1;
    if (a[0] == '-' && a[1] == 'r') do_rel = 1;
    ++i;
  }

  if (!dos_present()) { printf("dosenum: no C4DOS here\n"); return 1; }
  printf("dosenum: api version %d\n", dos_version());

  // v3 slot 15: the clock a transient cannot reach on its own without
  // leaving the base rung. Reported here because this is the program
  // that keeps the table honest, and because "the slot is advertised"
  // and "the slot answers" are different claims -- c4m depends on the
  // second one (src/c4dos/c4dos.c dos_api_time).
  if (!dos_can_time()) printf("dosenum: no clock available\n");
  else {
    n = dos_time();
    printf("dosenum: clock reads %d ms, %s\n", n,
           n > 0 ? "running" : "stopped");
  }

  if (!dos_can_enum()) { printf("dosenum: no enumeration available\n"); return 1; }

  n = dos_count();
  printf("dosenum: %d entries\n", n);
  i = 0;
  while (i < n) {
    nm = dos_entname(i);
    sz = dos_entsize(i);
    d = dos_entdata(i);
    // The first bytes matter: they are what proves ENTDATA points at
    // the content and not at some plausible-looking address.
    if (d && sz >= 3) printf("dosenum: [%d] %s %d bytes head=%c%c%c\n", i, nm, sz, d[0], d[1], d[2]);
    else printf("dosenum: [%d] %s %d bytes head=?\n", i, nm, sz);
    ++i;
  }

  if (do_trim) printf("dosenum: trim released %d bytes\n", dos_trim());
  if (do_rel)  printf("dosenum: release freed %d bytes, %d entries left\n",
                      dos_release(), dos_count());
  return 0;
}

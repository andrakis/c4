// dosres.c -- a resident image for dosexec's "resident" test: loaded
// once with C4DOS's slot 20, started twice with slot 21, unloaded with
// slot 22. Its globals outlive a start, its constructor runs at the
// load, its destructor at the unload.
//   ./c4cc -o dosres.c4r include/c4dos.h src/tests/dosres.c

int calls;
int ready;

int __attribute__((constructor)) dosres_init () { ready = 42; return 0; }
int __attribute__((destructor)) dosres_fini () {
  printf("dosres: destructor, after %d call(s)\n", calls);
  return 0;
}

int main (int argc, char **argv) {
  ++calls;
  printf("dosres: main call %d, the constructor said %d\n", calls, ready);
  if (argc > 1 && argv[1][0] == 'x') dos_exit(50 + calls);
  return calls;
}

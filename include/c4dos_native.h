// c4dos_native.h -- the C4DOS API, stubbed out for a host build.
//
// include/c4dos.h cannot be compiled by gcc: its invoke stub calls one
// function with varying argument counts, which is an ordinary c4 idiom
// and a hard error in C. A tool that wants to save a file under C4DOS
// therefore gets the real API when it is built for the machine
//   ./c4cc -o tool.c4r include/c4dos.h tool.c
// and these stubs when it is built for the host
//   gcc -include include/c4dos_native.h -o tool tool.c
// so the same source compiles both ways and reports honestly that
// there is no DOS here rather than pretending to write.
static int dos_present () { return 0; }
static int dos_can_write () { return 0; }
static int dos_create (char *name) { (void)name; return -1; }
static int dos_write (int h, char *buf, int len) { (void)h; (void)buf; (void)len; return 0; }
static int dos_close (int h) { (void)h; return 0; }
static int dos_put (char *name, char *buf, int len) { (void)name; (void)buf; (void)len; return -1; }
static int dos_readable () { return 0; }
static int dos_fopen (char *name) { (void)name; return -1; }
static int dos_fread (int h, char *buf, int len) { (void)h; (void)buf; (void)len; return 0; }
static int dos_fclose (int h) { (void)h; return 0; }
static int dos_slurp (char *name, char *buf, int max) { (void)name; (void)buf; (void)max; return -1; }
// v2: enumeration and memory hand-back. A host build has no RAM disk
// to enumerate and no DOS to give memory back to, so these report an
// empty, version-0 system -- the same honest answer the v1 stubs give.
static int dos_version () { return 0; }
static int dos_can_enum () { return 0; }
static int dos_count () { return 0; }
static char *dos_entname (int i) { (void)i; return 0; }
static int dos_entsize (int i) { (void)i; return -1; }
static char *dos_entdata (int i) { (void)i; return 0; }
static int dos_trim () { return 0; }
static int dos_release () { return 0; }

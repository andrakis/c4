// import.c -- a grin application's window kit (docs/grin-plan.md 3.4).
//
// Linked into an application in the kit's place, as Win16's import
// libraries were: a stub for each of the kit's functions (kitfns.h)
// that calls through the table grin publishes in C4DOS's slot 25
// (gkit.h). So calc.c, clock.c and notepad.c compile unchanged into the
// C4IX desktop, where these names are the kit itself, and into grin's
// applications, where they are these stubs. Compiled, like the
// application's other files, with -D WK_IMPORT: winkit.h's wk is then
// grin's, through wk_import_env.
//
// Memory. Every stub runs the kit in grin's heap generation (3.3) and
// switches back after: a window the kit opens for an application is
// grin's, as are its records. What the application allocates itself is
// its own -- including through wk_alloc, which is served here from its
// own libc, as the files are -- and goes back when grin unloads it.
//
// As it loads (its constructor), it finds grin's table and says hello:
// that is how grin tells an application from a DOS program it was asked
// to start. Under no grin there is no table, grin_present() says 0, and
// main had better say so and return.

#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include "winkit.h"
#include "grin.h"
#include "gkit.h"
#include "dospath.h"

extern int *__c4dos_api;              // bare.c's: C4DOS's table, written by its loader

enum { DOS_MAGIC = 4404292, SLOT_VERSION = 5, SLOT_SHELL = 25, SLOT_SETGEN = 27 };

struct wk_env *wk_import_env;
static struct grin_kit *T;

static struct grin_kit *table() {
    struct grin_kit *t;
    if (!__c4dos_api || __c4dos_api[0] != DOS_MAGIC || __c4dos_api[SLOT_VERSION] < 4) return 0;
    t = (struct grin_kit *)__c4dos_api[SLOT_SHELL];
    if (!t || t->magic != GRIN_MAGIC || t->version < GRIN_KIT_VERSION) return 0;
    return t;
}

int grin_present() { return T && table() == T; }

void __attribute__((constructor)) __grin_import() {
    if (!(T = table())) return;
    wk_import_env = T->env;
    T->hello();
}

// grin's generation for the call, and back after it
static int setgen(int g) {
    int (*f)(int);
    f = (int (*)(int))__c4dos_api[SLOT_SETGEN];
    return f(g);
}
static int gate() { return T->gen ? setgen(T->gen) : 0 - 1; }
static void ungate(int old) { if (old >= 0) setgen(old); }

#define K(r, n, p, a) r n p { r gk_r; int gk_old; gk_old = gate(); gk_r = T->n a; ungate(gk_old); return gk_r; }
#define V(n, p, a) void n p { int gk_old; gk_old = gate(); T->n a; ungate(gk_old); }
#include "kitfns.h"
#undef K
#undef V

// ---- the host's part, from the application's own libc ---------------------------

char *wk_alloc(int n) {
    char *p;
    if (n <= 0) n = 1;
    if ((p = (char *)malloc(n))) memset(p, 0, n);
    return p;
}
void wk_free(char *p) { if (p) free(p); }
// the kit's paths are grin's ("/B:/NOTE.TXT", dospath.h), DOS's names underneath
static char dospath[WK_PATH];
int wk_fopen(char *path, int wr) {
    return open(grin_dospath(path, dospath, WK_PATH), wr ? O_WRONLY | O_CREAT | O_TRUNC : O_RDONLY);
}
int wk_fread(int fd, char *buf, int n) { return read(fd, buf, n); }
int wk_fwrite(int fd, char *buf, int n) { return write(fd, buf, n); }
void wk_fclose(int fd) { close(fd); }

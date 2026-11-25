// wkgui.h -- the display's declarations (libjs/guest/gui.h) for every
// module but draw.c. gui.h brings gui.c's functions with it unless
// C4IX_SHARED says they come from C4IX's libgui.c4s; a program linked
// whole (the desktop by the host's Makefile, grin) must carry them once,
// so draw.c includes gui.h itself and the rest include this.

#ifdef C4IX_SHARED
#include "libjs/guest/gui.h"
#else
#define C4IX_SHARED 1
#include "libjs/guest/gui.h"
#undef C4IX_SHARED
#endif

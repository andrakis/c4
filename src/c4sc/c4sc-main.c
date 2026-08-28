// c4sc-main.c -- the driver as an OBJECT.
//
// Same body as the native host; the difference is that the seven
// generated units are declared rather than included, so c4lc -c turns
// every reference into an extern symbol and c4rlink resolves them
// against c4opt.c4o, c4lex.c4o, c4pp.c4o, c4parse.c4o, c4c4r.c4o,
// c4tree.c4o and c4gen.c4o. This unit carries the runtime -- c4sp's
// cells, collector, reader and builtins -- because that is what the
// generated units call and it has to live somewhere exactly once.
//
// The result is c4lc, compiled, as a .c4r: the thing M8 puts on the
// board.
#include "c4.h"
#include "c4m.h"
#include "c4_float.h"
// The C4DOS API, as c4sp.c selects it: stubs for a host build, the real
// table for an image, so a compiler running at the C4DOS rung can reach
// the RAM disk.
#ifdef C4SP_DOS
#include "c4dos.h"
#else
#include "c4dos_native.h"
#endif
#include "src/c4sp/include/cell.h"
#include "src/c4sp/include/gc.h"
#include "src/c4sp/include/cells.h"
#include "src/c4sp/include/atoms.h"
#include "src/c4sp/include/read.h"
#include "src/c4sp/include/stdlib.h"
#include "src/c4sp/include/eval.h"
#include "src/c4sc/scrt.h"
#include "src/c4sc/host.h"
#include "src/c4sc/body.h"

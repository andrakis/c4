//
// C4KE Standard Library: stddef.h
// Provides standard definitions.

#ifndef __c4cc__
// The host's, and through no guard of ours: a host header that wants one
// type from it says so first (#define __need_wint_t, as MSYS2's and
// Cygwin's do) and includes it again, and it has to get there every time.
#include_next <stddef.h>
#else  /* ifndef __c4cc__ */
#ifndef __STDDEF_H
#define __STDDEF_H

// Everything is an int
#define size_t    int

// c4lc and c4fc predefine __c4_inttypes__: they have real signed,
// unsigned, short and long, so the two defines below would only take
// them away (`unsigned long` would become `int`). c4cc has none of them.
#ifndef __c4_inttypes__
#ifndef int // Protect against c4/c4m's redefinition of int
#ifndef long
#define long      int
#endif
#endif

// unsigned is ignored under C4CC.
// Always use the full type name, ie `unsigned int` instead of just `unsigned`.
#define unsigned
#endif /* ifndef __c4_inttypes__ */

#endif /* ifndef __STDDEF_H */

#endif /* ifndef __c4cc__ */



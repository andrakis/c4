//
// C4KE Standard Library: stddef.h
// Provides standard definitions.

#ifndef __STDDEF_H
#define __STDDEF_H

#ifndef __c4cc__
#include_next <stddef.h>
#else  /* ifndef __c4cc__ */

// Everything is an int
#define size_t    int
#ifndef int // Protect against c4/c4m's redefinition of int
#ifndef long
#define long      int
#endif
#endif

// unsigned is ignored under C4CC.
// Always use the full type name, ie `unsigned int` instead of just `unsigned`.
#define unsigned

#endif /* ifndef __c4cc__ */

#endif /* ifndef __STDDEF_H */



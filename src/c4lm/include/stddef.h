//
// C4KE Standard Library: stddef.h
// Provides standard definitions.

#ifndef __STDDEF_H
#define __STDDEF_H

#if _WIN64
#define INT_TYPE long long
#elif _WIN32
#define INT_TYPE int
#else
#endif // if _WIN64

// Keep types the same across C4/gcc
#ifndef INT_TYPE
#define INT_TYPE long
#endif

#ifndef __c4cc__
#ifndef int
#define int INT_TYPE
#endif // #ifndef int
#endif // #ifndef __c4cc__

// Everything is an int.
//
// C4CC has no typedefs and no 'long' keyword: it parses any unknown identifier
// in a parameter list as a parameter *name*. So 'size_t size' expanding to
// 'long size' silently becomes TWO parameters, which is why functions such as
// vsnprintf() were seen by callers as taking 5 arguments instead of 4.
// Under C4CC these must therefore expand to 'int'.
#undef size_t
#undef ssize_t
#ifdef __c4cc__
#define size_t    int
#define ssize_t   int
// Same reasoning: 'long' is not a C4CC keyword.
#ifndef long
#define long      int
#endif
#else  /* ifdef __c4cc__ */
#define size_t    INT_TYPE
#define ssize_t   INT_TYPE
#endif /* ifdef/else __c4cc__ */

// unsigned is ignored under C4CC.
// Always use the full type name, ie `unsigned int` instead of just `unsigned`.
//#define unsigned
//#define signed

#endif /* ifndef __STDDEF_H */



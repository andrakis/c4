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

// Everything is an int
#undef size_t
#define size_t    INT_TYPE
#undef ssize_t
#define ssize_t   INT_TYPE

// unsigned is ignored under C4CC.
// Always use the full type name, ie `unsigned int` instead of just `unsigned`.
//#define unsigned
//#define signed

#endif /* ifndef __STDDEF_H */



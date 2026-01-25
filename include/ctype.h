//
// C4KE Standard Library: ctype.h
//

#ifndef __CTYPE_H
#define __CTYPE_H 1

#ifndef __c4cc__
#include_next <ctype.h>
#else /* ifndef __c4cc__ */

#ifndef __ctype_defined
#define __ctype_defined 1
#if NO_INLINE
int isnum (char c) { return c >= '0' && c <= '9'; }
int isspace (char c) { return c <= ' '; }
int isalpha (char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); }
int isalnum (char c) { return isnum(c) || isalpha(c); }
char tolower (char c) { return c | ' '; }
char toupper (char c) { return c & '_'; }
#else
#define isnum((c))   ((c) >= '0' && (c) <= '9')
#define isspace((c)) ((c) <= ' ')
#define isalpha((c)) (((c) >= 'A' && (c) <= 'Z') || ((c) >= 'a' && (c) <= 'z'))
#define isalnum((c)) (isnum((c)) || isalpha((c)))
#define tolower((c)) ((c) | ' ')
#define toupper((c)) ((c) & '_')
#endif /* if NO_INLINE/else */
#endif /* ifndef __ctype_defined */


#endif /* ifndef __c4cc__ */

#endif /* ifndef __CTYPE_H */

//
// C4 Standard Library: string.h
//

#ifndef __STDSTRING_H
#define __STDSTRING_H 1

#ifndef __c4cc__
#include_next <string.h>
// Rename to prevent gcc warnings
#define strlen renamed_strlen
int strlen(char *);
#undef strlen
#define strlen c4_strlen
//#define memcpy c4_memcpy
//#define memmove c4_memmove
#else /* ifndef __c4cc__ */

static int strlen (char *s) { int i; i = 0; while (*s++) ++i; return i; }
// static void *memcpy (void *source, void *dest, int length) {
// 	int   i;
// 	int  *is, *id;
// 	char *cs, *cd;
// 
// 	i = 0;
// 	if((int)dest   % sizeof(int) == 0 &&
// 	   (int)source % sizeof(int) == 0 &&
// 	   length % sizeof(int) == 0) {
// 		is = source; id = dest;
// 		length = length / sizeof(int);
// 		while (i < length) { id[i] = is[i]; ++i; }
// 	} else {
// 		cs = source; cd = dest;
// 		while (i < length) { cd[i] = cs[i]; ++i; }
// 	}
// 
// 	return dest;
// }
static void *memmove (void *source, void *dest, int length) {
	int   i;
	int  *is, *id;
	char *cs, *cd;

	if ((int)dest < (int)source)
		return memcpy(dest, source, length);

	i = length;
	if((int)dest   % sizeof(int) == 0 &&
	   (int)source % sizeof(int) == 0 &&
	   length % sizeof(int) == 0) {
		is = source; id = dest;
		length = length / sizeof(int);
		while (i > 0) { id[i - 1] = is[i - 1]; --i; }
	} else {
		cs = source; cd = dest;
		while (i > 0) { cd[i - 1] = cs[i - 1]; --i; }
	}

	return dest;
}

#endif /* ifndef __c4cc__ */
#endif /* ifndef __STDSTRING_H */

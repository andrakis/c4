//

#ifndef __STDLIB_H
#define __STDLIB_H 1

#ifndef __c4cc__
// Just provide definitions to quiet gcc.
void  exit   (int    code);
void  free   (void  *p);
void *malloc (int    size);
#endif

#endif // #ifndef __STDLIB_H

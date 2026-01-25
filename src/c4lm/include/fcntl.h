///
// C4 Standard Library - fcntl.h compatibility
///

#ifndef __FCNTL_H
#define __FCNTL_H 1

#ifndef __c4cc__
int open (char *pathname, int flags);
#endif // #ifndef __c4cc__

#endif // #ifndef __FCNTL_H

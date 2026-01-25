///
// C4 Standard Library - unistd.h compatibility
///

#ifndef __UNISTD_H
#define __UNISTD_H 1

#ifndef __c4cc__
ssize_t read (int fd, void *buf, size_t count);
int     close (int fd);
#endif // #ifndef __c4cc__

#endif // #ifndef __UNISTD_H


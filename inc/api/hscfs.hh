#pragma once

#include <sys/types.h>

#define O_RDONLY	     00
#define O_WRONLY	     01
#define O_RDWR		     02

#ifndef O_CREAT
# define O_CREAT	   0100	/* Not fcntl.  */
#endif
#ifndef O_TRUNC
# define O_TRUNC	  01000	/* Not fcntl.  */
#endif
#ifndef O_APPEND
# define O_APPEND	  02000
#endif

namespace hscfs {

int open(const char *pathname, int flags);
int close(int fd);
ssize_t read(int fd, void *buffer, size_t count);
int truncate(int fd, off_t length);
int unlink(const char *pathname);
ssize_t write(int fd, void *buffer, size_t count);

}  // namespace hscfs
#pragma once

#include <sys/types.h>

namespace hscfs {

int open(const char *pathname, int flags);
ssize_t read(int fd, void *buffer, size_t count);

}  // namespace hscfs
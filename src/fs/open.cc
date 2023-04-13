#include "cache/super_cache.hh"
#include "fs/fs_manager.hh"

extern "C" int open(const char *pathname, int flags, ...);

int open(const char *pathname, int flags, ...)
{
    return 0;
}
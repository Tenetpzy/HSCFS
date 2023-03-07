#include <stdio.h>
#include <stdarg.h>
#include <errno.h>
#include <string.h>

#include "utils/hscfs_log.h"

const char *log_level_str[] = {"DEBUG", "INFO", "WARING", "ERROR"};
#define ERRNO_MSG_LEN 128
#define PRINT_TO_STDERR(fmt, ...) \
    vfprintf(stderr, fmt, ##__VA_ARGS__)

void hscfs_log_print(hscfs_log_level log_level, const char *funcname, unsigned int lineno, const char *fmt, ...)
{
    fprintf(stderr, "[%s:%u, %s]: ", funcname, lineno, log_level_str[log_level]);

    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    fprintf(stderr, "\n");
    va_end(args);
}

void hscfs_log_errno(hscfs_log_level log_level, const char *funcname, unsigned int lineno, int err, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    hscfs_log_print(log_level, funcname, lineno, fmt, args);
    va_end(args);
    
    char errno_msg[ERRNO_MSG_LEN];
    if (strerror_r(err, errno_msg, sizeof(errno_msg)) == 0)
        fprintf(stderr, "error: %s\n", errno_msg);
}
#include "utils/hscfs_log.h"
#include <stdio.h>
#include <stdarg.h>
#include <errno.h>
#include <string.h>

const char *log_level_str[] = {"DEBUG", "INFO", "WARING", "ERROR"};
#define ERRNO_MSG_LEN 128
#define PRINT_TO_STDERR(fmt, ...) \
    fprintf(stderr, fmt, ##__VA_ARGS__)

void hscfs_log_print(hscfs_log_level log_level, const char *funcname, unsigned int lineno, const char *fmt, ...)
{
    PRINT_TO_STDERR("[%s:%u, %s]: ", funcname, lineno, log_level_str[log_level]);

    va_list args;
    va_start(args, fmt);
    PRINT_TO_STDERR(fmt, args);
    PRINT_TO_STDERR("\n");
    va_end(args);
}

void hscfs_log_errno(hscfs_log_level log_level, const char *funcname, unsigned int lineno, int err, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    hscfs_log_print(log_level, funcname, lineno, fmt, args);
    va_end(args);
    
    char errno_msg[ERRNO_MSG_LEN];
    char *msg = strerror_r(err, errno_msg, sizeof(errno_msg));
    PRINT_TO_STDERR("error: %s\n", msg);
}
#pragma once

typedef enum hscfs_log_level 
{
    HSCFS_LOG_DEBUG, HSCFS_LOG_INFO, HSCFS_LOG_WARNING, HSCFS_LOG_ERROR
} hscfs_log_level;

void hscfs_log_print(hscfs_log_level log_level, const char *funcname, unsigned int lineno, const char *fmt, ...);
void hscfs_log_errno(hscfs_log_level log_level, const char *funcname, unsigned int lineno, int err, const char *fmt, ...);

#define HSCFS_LOG(log_level, format, ...) \
    hscfs_log_print(log_level, __func__, __LINE__, format, ##__VA_ARGS__)

#define HSCFS_LOG_ERRNO(log_level, err, format, ...) \
    hscfs_log_errno(log_level, __func__, __LINE__, err, format, ##__VA_ARGS__)

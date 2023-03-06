#ifndef __HSCFS_TYPES_H__
#define __HSCFS_TYPES_H__

#ifdef FOR_SSD_FIRMWARE

typedef u32 __le32;
typedef u8  __u8;
typedef u64 __le64;
typedef u16 __le16;

#else

#include <stdint.h>
#include <stddef.h>
#include <linux/stat.h>

typedef uint32_t    u32;
typedef uint64_t    u64;
typedef uint16_t    u16;
typedef uint8_t     u8;

#endif

#define FS_OK   0
#define FS_FAIL 0x7f

#define BITS_PER_BYTE_SHIFT 3

#endif
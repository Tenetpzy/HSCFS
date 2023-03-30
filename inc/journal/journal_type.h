#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "fs/fs.h"

enum
{
    JOURNAL_TYPE_NATS,
    JOURNAL_TYPE_SITS,
    JOURNAL_TYPE_SUPER_BLOCK,
    JOURNAL_TYPES,
    JOURNAL_TYPE_NOP = 0x7e,
    JOURNAL_TYPE_END
};

struct meta_journal_entry
{
    u16 len;
    u8 type;
    u8 rsv;
    u8 journal_data[0];
} __attribute__((packed));

typedef struct meta_journal_entry meta_journal_entry;

struct NAT_journal_entry
{
    u32 nid;
    struct hscfs_nat_entry newValue;
} __attribute__((packed));

typedef struct NAT_journal_entry NAT_journal_entry;

struct SIT_journal_entry
{
    u32 segID;
    struct hscfs_sit_entry newValue;
} __attribute__((packed));

typedef struct SIT_journal_entry SIT_journal_entry;

struct super_block_journal_entry
{
    u32 Off;
    u32 newVal;
} __attribute__((packed));

typedef struct super_block_journal_entry super_block_journal_entry;

#ifdef __cplusplus
}
#endif
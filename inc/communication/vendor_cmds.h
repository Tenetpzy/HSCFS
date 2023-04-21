#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "fs/fs.h"

struct migrate_task
{
    uint32_t migrate_lpa_cnt;
    struct hscfs_sit_entry victim_seg_info;
    uint64_t migrate_dst_lpa;
    uint64_t migrate_src_lpa;
}__attribute__((packed));
typedef struct migrate_task migrate_task;

/*例：从根目录开始查询/home/huawei/hisilicon/ssd
  startIno为/的ino
  path为home/huawei/hisilicon/ssd
  depth为4
*/
struct path_lookup_task
{
    u32 start_ino;   /*起始目录的ino*/
    u32 pathlen;    /*路径字符串的长度，不含0结束符*/
    u32 depth;      /*路径的级数*/
    char path[0];   /*路径字符串，不含0结束符*/
}__attribute__((packed));
typedef struct path_lookup_task path_lookup_task;

#define MAX_PATH_DEPTH  ((4096 - 12) / sizeof(u32))
struct path_lookup_result
{
    u64 dentry_blkidx;               /*若目标文件存在，该项表示对应dentry在父目录文件的哪个block中(块偏移)*/
                                    /*若目标文件不存在，但其父目录存在，并且dentryBitPos不为INVALID_DENTRY_BITPOS，
                                      该字段表示若要创建此文件，新dentry所在的block*/
    u32 dentry_bitpos;               /*若目标文件存在，该项表示对应dentry在block中的位置(slot号)*/
                                    /*若目标文件不存在，但其父目录存在，此字段表示若要创建此文件，新dentry在block中的偏移*/

    u32 path_inos[MAX_PATH_DEPTH];   /*路径各级的ino，INVALID_NID表示该级文件不存在*/


    char parent_dir_node_page[4096];   /*若目标文件存在，该项是索引对应dentry所在的block的node page的内容*/
    char parent_dir_data_page[4096];   /*若目标文件存在，该项是存放对应dentry的data block的内容*/
}__attribute__((packed));
typedef struct path_lookup_result path_lookup_result;

struct filemapping_search_task
{
    u32 ino;
    u32 nid_to_start;
    u64 file_blk_offset;
    u8 return_all_Level;
};
typedef struct filemapping_search_task filemapping_search_task;

#define VENDOR_SET_OPCODE 0xc5
#define VENDOR_GET_OPCODE 0xc2

#ifdef __cplusplus
}
#endif
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "utils/types.h"

struct hscfs_super_block
{
    /*stable read only field*/
    __le32 magic;			/* Magic Number */
	__le16 major_ver;		/* Major Version */
	__le16 minor_ver;		/* Minor Version */
	__le32 log_sectorsize;		/* log2 sector size in bytes */
	__le32 log_sectors_per_block;	/* log2 # of sectors per block */
	__le32 log_blocksize;		/* log2 block size in bytes */
	__le32 log_blocks_per_seg;	/* log2 # of blocks per segment */
	__le64 block_count;		/* total # of user blocks(used) */
	__le32 segment_count;		/* total # of segments(used) */
	__le32 segment_count_sit;	/* # of segments for SIT(used) */
	__le32 segment_count_nat;	/* # of segments for NAT(used) */
	__le32 segment_count_srmap;	/* # of segments for SRMAP */
    __le32 segment_count_meta_journal;  /* # of segments for Meta Journal*/
	__le32 segment_count_main;	/* # of segments for main area */
	__le32 segment0_blkaddr;	/* start block address of segment 0(used) */
	__le32 sit_blkaddr;		/* start block address of SIT(used) */
	__le32 nat_blkaddr;		/* start block address of NAT(used) */
	__le32 srmap_blkaddr;		/* start block address of SRMAP(used) */
    __le32 meta_journal_blkaddr;   /* start block address of Meta Journal */
	__le32 main_blkaddr;		/* start block address of main area */
	__le32 root_ino;		/* root inode number(used) */
	__le32 node_ino;		/* node inode number */
	__le32 meta_ino;		/* meta inode number */

    /*frequently changed field, cached in host DRAM and SSD DB*/
    __le32 first_free_segment_id;
    __le32 first_data_segment_id;
    __le32 first_node_segment_id;
    __le32 current_data_segment_id;
    __le32 current_data_segment_blkoff;  // 下一个可用块的segment内块偏移
    __le32 current_node_segment_id;
    __le32 current_node_segment_blkoff;
    __le16 meta_journal_start_blkoff;
    __le16 meta_journal_end_blkoff;  // 尾后块偏移
    __le32 free_segment_count;
    __le32 next_free_nid;  // 空闲nid链表，链表尾为INVALID_NID

	__u8 reserved[3964];		/* valid reserved region */
};

#define BLOCK_PER_SEGMENT 512

#define INVALID_LPA	0
#define INVALID_SEGID 0  // 将超级块segment id用作无效的node/data/free segid

#define HSCFS_NAME_LEN		255
#define DEF_ADDRS_PER_INODE	932	/* Address Pointers in an Inode */
#define CUR_ADDRS_PER_INODE(inode)	DEF_ADDRS_PER_INODE
#define DEF_NIDS_PER_INODE	5	/* Node IDs in an Inode */
#define ADDRS_PER_INODE(inode)	DEF_ADDRS_PER_INODE
#define DEF_ADDRS_PER_BLOCK	1020	/* Address Pointers in a Direct Block */
#define ADDRS_PER_BLOCK(inode)	DEF_ADDRS_PER_BLOCK
#define NIDS_PER_BLOCK		1020	/* Node IDs in an Indirect Block */

#define ADDRS_PER_PAGE(page, inode)	\
	(IS_INODE(page) ? ADDRS_PER_INODE(inode) : ADDRS_PER_BLOCK(inode))

#define	NODE_DIR1_BLOCK		(DEF_ADDRS_PER_INODE + 1)
#define	NODE_DIR2_BLOCK		(DEF_ADDRS_PER_INODE + 2)
#define	NODE_IND1_BLOCK		(DEF_ADDRS_PER_INODE + 3)
#define	NODE_IND2_BLOCK		(DEF_ADDRS_PER_INODE + 4)
#define	NODE_DIND_BLOCK		(DEF_ADDRS_PER_INODE + 5)

#define MAX_FILE_MAPPING_LEVEL	4
#define INVALID_NID				0

#define HSCFS_INLINE_DATA	0x02	/* file inline data flag */
#define HSCFS_INLINE_DENTRY	0x04	/* file inline dentry flag */
#define HSCFS_DATA_EXIST		0x08	/* file inline data exist flag */
#define HSCFS_INLINE_DOTS	0x10	/* file having implicit dot dentries */

#define MAX_INLINE_DATA		(sizeof(__le32) * (DEF_ADDRS_PER_INODE))

struct hscfs_inode {
	__le16 i_mode;			/* file mode，暂不使用 */
	__u8 i_inline;			/* file inline flags */
    __u8 i_rsv0;
	__le32 i_type;			/* user ID，不使用，用作文件类型字段(used) */
	__le32 i_nlink;			/* group ID，不使用，用作硬链接计数字段(used) */
    __le32 i_atime_nsec;		/* access time in nano scale(used) */
	__le32 i_ctime_nsec;		/* change time in nano scale */
	__le32 i_mtime_nsec;		/* modification time in nano scale(used) */
	__le64 i_size;			/* file size in bytes(used) */
	__le64 i_blocks;		/* file size in blocks */
	__le64 i_atime;			/* access time(used) */
	__le64 i_dentry_num;	/* change time，不使用，用作目录文件中当前dentry数目字段(rmdir使用) */
	__le64 i_mtime;			/* modification time(used) */

	/* 
	 * only for directory depth，值为当前目录文件的哈希表最大下标（即哈希表个数-1）
	 * 哈希表下标从0开始（只要目录文件存在，至少有1个哈希表）(used) 
	 */
	__le32 i_current_depth;
	__le32 i_pino;			/* parent inode number */
	__le32 i_namelen;		/* file name length */
	__u8 i_name[HSCFS_NAME_LEN];	/* file name for SPOR */
	__u8 i_dir_level;		/* dentry_level for large dir(used，始终置为0) */


	__le32 i_addr[DEF_ADDRS_PER_INODE];	/* Pointers to data blocks */
	__le32 i_nid[DEF_NIDS_PER_INODE];	/* direct(2), indirect(2),
						double_indirect(1) node id */
};

#define SIZE_TO_BLOCK(size) (((size) + 4095) >> 12)

struct direct_node {
	__le32 addr[DEF_ADDRS_PER_BLOCK];	/* array of data block address */
};

struct indirect_node {
	__le32 nid[NIDS_PER_BLOCK];	/* array of data block address */
};

struct node_footer {
	__le32 nid;		/* node id */
	__le32 ino;		/* inode number */
	__le32 offset;		/*  offset */
	__le32 next_blkaddr;	/* next node page block address(不使用) */
};

struct hscfs_node {
	/* can be one of three types: inode, direct, and indirect types */
	union {
		struct hscfs_inode i;
		struct direct_node dn;
		struct indirect_node in;
	};
	struct node_footer footer;
};

struct hscfs_nat_entry {
	__le32 ino;		/* inode number */  // 若nid = ino，则该node为inode
	__le32 block_addr;	/* block address */
} __attribute__((packed));

#define NAT_ENTRY_PER_BLOCK (4096 / sizeof(struct hscfs_nat_entry))

struct hscfs_nat_block {
	struct hscfs_nat_entry entries[NAT_ENTRY_PER_BLOCK];
} __attribute__((packed));

/*
 * For SIT entries
 *
 * Each segment is 2MB in size by default so that a bitmap for validity of
 * there-in blocks should occupy 64 bytes, 512 bits.
 * Not allow to change this.
 */
#define SIT_VBLOCK_MAP_SIZE 64

#define SEG_BLK_OFF_MASK	((1ul << 9) - 1)

/*
 * HSCFS uses 4 bytes to represent block address. As a result, supported size of
 * disk is 16 TB and it equals to 16 * 1024 * 1024 / 2 segments.
 */
#define HSCFS_MAX_SEGMENT       ((16 * 1024 * 1024) / 2)

/*
 * Note that hscfs_sit_entry->vblocks has the following bit-field information.
 * [31:9] : next segment id
 * [8:0] : valid block count
 */
#define SIT_VBLOCKS_SHIFT	9
#define SIT_VBLOCKS_MASK	((1 << SIT_VBLOCKS_SHIFT) - 1)
#define GET_SIT_VBLOCKS(raw_sit)				\
	(((raw_sit)->vblocks) & SIT_VBLOCKS_MASK)
#define GET_NEXT_SEG(raw_sit)					\
	((((raw_sit)->vblocks) & ~SIT_VBLOCKS_MASK)	\
	 >> SIT_VBLOCKS_SHIFT)
#define SET_NEXT_SEG(raw_sit, next_seg)         \
    do  \
    { \
        ((raw_sit)->vblocks) |= ((next_seg) << SIT_VBLOCKS_SHIFT);   \
    }while(0)

struct hscfs_sit_entry {
	__le32 vblocks;				/* reference above */
	__u8 valid_map[SIT_VBLOCK_MAP_SIZE];	/* bitmap for valid blocks */
} __attribute__((packed));

#define SIT_ENTRY_PER_BLOCK (4096 / sizeof(struct hscfs_sit_entry))

struct hscfs_sit_block {
	struct hscfs_sit_entry entries[SIT_ENTRY_PER_BLOCK];
} __attribute__((packed));

/*
 * For segment summary
 *
 * One summary block contains exactly 512 summary entries, which represents
 * exactly 2MB segment by default. Not allow to change the basic units.
 *
 * NOTE: For initializing fields, you must use set_summary
 *
 * - If data page, nid represents dnode's nid
 * - If node page, nid represents the node page's nid.
 *
 * The ofs_in_node is used by only data page. It represents offset
 * from node's page's beginning to get a data block address.
 * ex) data_blkaddr = (block_t)(nodepage_start_address + ofs_in_node)
 */
#define ENTRIES_IN_SUM		512
#define	SUMMARY_SIZE		8	/* sizeof(struct summary) */

/* a summary entry for a 4KB-sized block in a segment */
/* 
 * 对于node block，nid字段为它的nid，ofs_in_node无效
 * 
 * 对于data block，nid字段为所属文件inode号(而不是直接索引该data block的node block)
 * ofs_in_node实际保存块偏移(而不是索引node block内的索引项偏移)
 * 这么做的原因：node block的淘汰保护，要求非叶节点必须在缓存中，
 * 因此不能只记录直接索引的node block，这样在垃圾回收修改node block时无法满足前述要求，会出现叶节点在缓存而非叶节点不在的情况。
 */
struct hscfs_summary {
	__le32 nid;		/* parent node id */
	__le32 ofs_in_node;	/* block index in parent node */
} __attribute__((packed));

/* 4KB-sized summary block structure */
struct hscfs_summary_block {
	struct hscfs_summary entries[ENTRIES_IN_SUM];
} __attribute__((packed));

/*
 * For directory operations
 */
#define HSCFS_DOT_HASH		0
#define HSCFS_DDOT_HASH		HSCFS_DOT_HASH
#define HSCFS_MAX_HASH		(~((0x3ULL) << 62))
#define HSCFS_HASH_COL_BIT	((0x1ULL) << 63)

typedef __le32	hscfs_hash_t;

/* One directory entry slot covers 8bytes-long file name */
#define HSCFS_SLOT_LEN		8
#define HSCFS_SLOT_LEN_BITS	3

#define GET_DENTRY_SLOTS(x) (((x) + HSCFS_SLOT_LEN - 1) >> HSCFS_SLOT_LEN_BITS)

/* MAX level for dir lookup */
#define MAX_DIR_HASH_DEPTH	63

/* MAX buckets in one level of dir */
#define MAX_DIR_BUCKETS		(1 << ((MAX_DIR_HASH_DEPTH / 2) - 1))

/*
 * space utilization of regular dentry and inline dentry (w/o extra reservation)
 *		regular dentry		inline dentry (def)	inline dentry (min)
 * bitmap	1 * 27 = 27		1 * 25 = 25		1 * 1 = 1
 * reserved	1 * 3 = 3		1 * 17 = 17		1 * 1 = 1
 * dentry	11 * 214 = 2354		11 * 194 = 2134		11 * 2 = 22
 * filename	8 * 214 = 1712		8 * 194 = 1552		8 * 2 = 16
 * total	4096			3728			40
 *
 * Note: there are more reserved space in inline dentry than in regular
 * dentry, when converting inline dentry we should handle this carefully.
 */
#define NR_DENTRY_IN_BLOCK	214	/* the number of dentry in a block */
#define SIZE_OF_DIR_ENTRY	11	/* by byte */
#define SIZE_OF_DENTRY_BITMAP	((NR_DENTRY_IN_BLOCK + 7) / \
					8)
#define SIZE_OF_RESERVED	(4096 - ((SIZE_OF_DIR_ENTRY + \
				HSCFS_SLOT_LEN) * \
				NR_DENTRY_IN_BLOCK + SIZE_OF_DENTRY_BITMAP))
#define MIN_INLINE_DENTRY_SIZE		40	/* just include '.' and '..' entries */

#define INVALID_DENTRY_BITPOS	(NR_DENTRY_IN_BLOCK + 1)

/* One directory entry slot representing HSCFS_SLOT_LEN-sized file name */
struct hscfs_dir_entry {
	__le32 hash_code;	/* hash code of file name */
	__le32 ino;		/* inode number */
	__le16 name_len;	/* length of file name */
	__u8 file_type;		/* file type */
} __attribute__((packed));

/* 4KB-sized directory entry block */
struct hscfs_dentry_block {
	/* validity bitmap for directory entries in each block */
	__u8 dentry_bitmap[SIZE_OF_DENTRY_BITMAP];
	__u8 reserved[SIZE_OF_RESERVED];
	struct hscfs_dir_entry dentry[NR_DENTRY_IN_BLOCK];
	__u8 filename[NR_DENTRY_IN_BLOCK][HSCFS_SLOT_LEN];
} __attribute__((packed));

/* for inline dir */
#define NR_INLINE_DENTRY	(MAX_INLINE_DATA * 8 / \
				((SIZE_OF_DIR_ENTRY + HSCFS_SLOT_LEN) * \
				8 + 1))
#define INLINE_DENTRY_BITMAP_SIZE	((NR_INLINE_DENTRY + \
					8 - 1) / 8)
#define INLINE_RESERVED_SIZE	(MAX_INLINE_DATA - \
				((SIZE_OF_DIR_ENTRY + HSCFS_SLOT_LEN) * \
				NR_INLINE_DENTRY + INLINE_DENTRY_BITMAP_SIZE))

/* inline directory entry structure */
struct hscfs_inline_dentry {
	__u8 dentry_bitmap[INLINE_DENTRY_BITMAP_SIZE];
	__u8 reserved[INLINE_RESERVED_SIZE];
	struct hscfs_dir_entry dentry[NR_INLINE_DENTRY];
	__u8 filename[NR_INLINE_DENTRY][HSCFS_SLOT_LEN];
} __attribute__((packed));

/* file types used in inode_info->flags */
enum {
	HSCFS_FT_UNKNOWN,
	HSCFS_FT_REG_FILE,
	HSCFS_FT_DIR,
	HSCFS_FT_CHRDEV,
	HSCFS_FT_BLKDEV,
	HSCFS_FT_FIFO,
	HSCFS_FT_SOCK,
	HSCFS_FT_SYMLINK,
	HSCFS_FT_MAX
};

#ifdef __cplusplus
}
#endif
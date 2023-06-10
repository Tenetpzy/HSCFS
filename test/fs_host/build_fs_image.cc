#include "fs/fs.h"

#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdexcept>
#include <cstring>
#include <cstdlib>
#include <string>

/*
 * 文件系统测试镜像
 *
 * *********************************************************************
 * 
 * 布局
 * 总共1034个block，其中：
 * blkno: 0          1   2   3           7     10          522         1034
 * area:  SuperBlock SIT NAT MetaJournal SRMAP NodeSegment DataSegment (没有FreeSegment)
 * 
 * *********************************************************************
 * 
 * 初始状态下，包含的文件和目录：
 * /a/b/c
 * 根目录：inode = 2，包含1个node block(lpa = 10)，1个data block(lpa = 522)，含有目录项a
 * 
 * 目录a：inode = 3，包含1个node block(lpa = 11)，1个data block(lpa = 523)，含有目录项b
 * 
 * 目录b：inode = 4，包含1个node block(lpa = 12)，1个data block(lpa = 524)，含有目录项c
 * 
 * 文件c：inode = 5，包含1个node block(lpa = 13)，1个data block(lpa = 525)。
 * 初始数据为：从偏移0开始的字符串"hello hscfs!"
 * 
 */

void do_write(int fd, void *buffer, size_t count)
{
    if (write(fd, buffer, count) < ssize_t(count))
        throw std::runtime_error("write error");
}

void do_lseek(int fd, off_t offset, int whence)
{
    if (lseek(fd, offset, whence) == -1)
        throw std::runtime_error("lseek error");
}

void generate_super_block(void *buffer)
{
    hscfs_super_block *super = static_cast<hscfs_super_block*>(buffer);
    memset(super, 0, sizeof(hscfs_super_block));
    super->block_count = 1034;
    super->segment_count = 2;
    super->segment_count_sit = 1; // 代码中只用来做正确性判断，此处写1不影响，以下同理
    super->segment_count_nat = 1;
    super->segment_count_srmap = 1;
    super->segment_count_meta_journal = 1;
    super->segment_count_main = 2;
    super->segment0_blkaddr = 10; // NodeSegment起始位置
    super->sit_blkaddr = 1;
    super->nat_blkaddr = 2;
    super->srmap_blkaddr = 7;
    super->meta_journal_blkaddr = 3;
    super->main_blkaddr = 10;
    super->root_ino = 2;

    super->first_free_segment_id = HSCFS_MAX_SEGMENT;
    super->first_data_segment_id = HSCFS_MAX_SEGMENT;
    super->first_node_segment_id = HSCFS_MAX_SEGMENT;
    super->current_data_segment_id = 1;
    super->current_data_segment_blkoff = 4;
    super->current_node_segment_id = 0;
    super->current_node_segment_blkoff = 4;
    super->meta_journal_start_blkoff = 3;
    super->meta_journal_end_blkoff = 7;
    super->free_segment_count = 0;
    super->next_free_nid = 6;
}

void generate_sit(void *buffer)
{
    hscfs_sit_block *sit = static_cast<hscfs_sit_block*>(buffer);
    memset(sit, 0, sizeof(hscfs_sit_block));

    /* node 和 data segment内前4个block有效，位图中首元素为1111b */
    for (int i = 0; i < 2; ++i)
    {
        sit->entries[i].valid_map[0] = 15;
        sit->entries[i].vblocks = 4;
    }
}

void generate_nat(void *buffer)
{
    hscfs_nat_block *nat = static_cast<hscfs_nat_block*>(buffer);
    memset(nat, 0, sizeof(hscfs_nat_block));
    for (uint32_t i = 0, ino_base = 2, lpa_base = 10; i < 4; ++i)
    {
        uint32_t nid = ino_base + i;
        nat->entries[nid].ino = nid;
        nat->entries[nid].block_addr = lpa_base + i;
    }
    const uint32_t nat_entry_num = sizeof(hscfs_nat_block) / sizeof(hscfs_nat_entry);
    for (uint32_t nid = 6; nid < nat_entry_num; ++nid)
    {
        nat->entries[nid].ino = 0;
        nat->entries[nid].block_addr = nid + 1;
    }
    nat->entries[nat_entry_num - 1].block_addr = INVALID_NID;
}

uint32_t hscfs_dentry_hash(const char *name, u32 len)
{
    auto str2hashbuf = [](const unsigned char *msg, u32 len, u32 *buf, int num)
    {
        u32 pad, val;
        u32 i;

        pad = len | (len << 8);
        pad |= pad << 16;

        val = pad;
        if (len > (u32)num * 4)
            len = num * 4;
        for (i = 0; i < len; i++) {
            if ((i % 4) == 0)
                val = pad;
            val = msg[i] + (val << 8);
            if ((i % 4) == 3) {
                *buf++ = val;
                val = pad;
                num--;
            }
        }
        if (--num >= 0)
            *buf++ = val;
        while (--num >= 0)
            *buf++ = pad;
    };

    auto TEA_transform = [](u32 buf[4], u32 in[])
    {
        u32 sum = 0;
        u32 b0 = buf[0], b1 = buf[1];
        u32 a = in[0], b = in[1], c = in[2], d = in[3];
        int n = 16;

        do {
            sum += 0x9E3779B9;
            b0 += ((b1 << 4)+a) ^ (b1+sum) ^ ((b1 >> 5)+b);
            b1 += ((b0 << 4)+c) ^ (b0+sum) ^ ((b0 >> 5)+d);
        } while (--n);

        buf[0] += b0;
        buf[1] += b1;
    };

    u32 hash;
	u32 hscfs_hash;
	const unsigned char *p;
	u32 in[8], buf[4];

	if ((len == 1 && name[0] == '.') ||
        (len == 2 && name[0] == '.' && name[1] == '.'))
		return 0;

	/* Initialize the default seed for the hash checksum functions */
	buf[0] = 0x67452301;
	buf[1] = 0xefcdab89;
	buf[2] = 0x98badcfe;
	buf[3] = 0x10325476;

	p = reinterpret_cast<const unsigned char*>(name);
	while (1) {
		str2hashbuf(p, len, in, 4);
		TEA_transform(buf, in);
		p += 16;
		if (len <= 16)
			break;
		len -= 16;
	}
	hash = buf[0];
	hscfs_hash = hash & ~HSCFS_HASH_COL_BIT;
	return hscfs_hash;
}

void write_dentry_in_dir_block(hscfs_dentry_block *blk, const std::string &name, uint8_t type, uint32_t ino)
{
    auto set_bitmap_pos = [](unsigned long slot_pos, void *bitmap_start_addr)
    {
        const uint64_t BITS_PER_LONG = 64;
        auto start_addr = static_cast<uint64_t*>(bitmap_start_addr);
        uint64_t idx = slot_pos / BITS_PER_LONG;
        uint64_t off = slot_pos & (BITS_PER_LONG - 1U);
        start_addr[idx] |= (1U << off);
    };

    size_t name_len = name.length();

    uint32_t start_slot = 0;
    uint32_t occupy_slot_num = GET_DENTRY_SLOTS(name_len);
    uint32_t hash_code = hscfs_dentry_hash(name.c_str(), name_len);

    uint8_t *name_store_addr = blk->filename[start_slot];
    uint8_t *bitmap_addr = blk->dentry_bitmap;
    hscfs_dir_entry *dentry_ptr = &blk->dentry[start_slot];

    /* 设置位图 */
    for (uint32_t i = 0; i < occupy_slot_num; ++i)
        set_bitmap_pos(start_slot + i, bitmap_addr);

    /* 设置目录项 */
    dentry_ptr->hash_code = hash_code;
    dentry_ptr->file_type = type;
    dentry_ptr->ino = ino;
    dentry_ptr->name_len = name_len;

    /* 设置文件名 */
    std::memcpy(name_store_addr, name.c_str(), name_len);
}

void write_dir_and_file(int fd)
{
    char buffer[4096];
    char test_file_content[] = "hello hscfs!";
    auto node = reinterpret_cast<hscfs_node*>(buffer);

    /* 写node blocks */
    for (uint32_t i = 0, ino_base = 2, node_lpa_base = 10, data_lpa_base = 522; i < 4; ++i)
    {
        memset(node, 0, sizeof(hscfs_node));
        uint32_t ino = ino_base + i;
        node->footer.ino = ino;
        node->footer.nid = ino;
        node->footer.offset = 0;

        hscfs_inode *inode = &node->i;
        inode->i_type = i == 3 ? HSCFS_FT_REG_FILE : HSCFS_FT_DIR;
        inode->i_nlink = 1;
        inode->i_size = i == 3 ? sizeof(test_file_content) : 4096 * 2;
        inode->i_dentry_num = 1;
        inode->i_current_depth = 0;
        inode->i_addr[0] = data_lpa_base + i;

        uint32_t wr_offset = (node_lpa_base + i) * 4096;
        do_lseek(fd, wr_offset, SEEK_SET);
        do_write(fd, node, sizeof(hscfs_node));
    }

    /* 写data blocks */
    auto dir_blk = reinterpret_cast<hscfs_dentry_block*>(buffer);
    std::string dentrys[3] = {"a", "b", "c"};
    for (uint32_t i = 0, data_lpa_base = 522, ino_base = 3; i < 3; ++i)
    {
        memset(dir_blk, 0, sizeof(hscfs_dentry_block));
        write_dentry_in_dir_block(dir_blk, dentrys[i], i == 2 ? HSCFS_FT_REG_FILE : HSCFS_FT_DIR, ino_base + i);
        uint32_t wr_offset = (data_lpa_base + i) * 4096;
        do_lseek(fd, wr_offset, SEEK_SET);
        do_write(fd, dir_blk, sizeof(hscfs_dentry_block));
    }

    memset(buffer, 0, 4096);
    memcpy(buffer, test_file_content, sizeof(test_file_content));
    do_lseek(fd, 525 * 4096, SEEK_SET);
    do_write(fd, buffer, 4096);
}

void build_test_image(int fd)
{
    char buffer[4096];

    generate_super_block(buffer);
    do_lseek(fd, 0, SEEK_SET);
    do_write(fd, buffer, 4096);

    generate_sit(buffer);
    do_lseek(fd, 1 * 4096, SEEK_SET);
    do_write(fd, buffer, 4096);

    generate_nat(buffer);
    do_lseek(fd, 2 * 4096, SEEK_SET);
    do_write(fd, buffer, 4096);

    write_dir_and_file(fd);   
}

int main()
{
    int fd = open("./fsImage", O_WRONLY | O_CREAT | O_TRUNC, S_IRWXU);
    if (fd == -1)
        throw std::runtime_error("open error");
    build_test_image(fd);
    if (fsync(fd) == -1)
        throw std::runtime_error("fsync error");
    if (close(fd) == -1)
        throw std::runtime_error("close error");
    return 0;
}
#include "spdk/env.h"
#include "spdk/nvme.h"
#include "communication/dev.h"
#include "communication/channel.h"
#include "communication/session.h"
#include "communication/memory.h"
#include "communication/comm_api.h"
#include "fs/fs.h"

#include <string>
#include <cstring>
#include <utility>
#include <tuple>
#include <ctime>

namespace hscfs {

/* api/init.cc中的定义 */
struct Device_Env
{
    spdk_nvme_transport_id trid;
    comm_dev dev;
};

extern Device_Env device_env;

int device_env_init();
std::string parse_trid_from_argv(int argc, char *argv[]);
bool probe_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid, struct spdk_nvme_ctrlr_opts *opts);
void attach_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
	  struct spdk_nvme_ctrlr *_ctrlr, const struct spdk_nvme_ctrlr_opts *opts);
int spdk_init(const std::string &trid);

}  // namespace hscfs

namespace hscfs {

#define LPA_SIZE 4096
#define ROOT_INO 2
#define META_JOURNAL_BLK_CNT 1024   // meta journal默认占用1024个块
#define UPPER_BOUND(x, y)  (((x) + (y) - 1) / (y))

static uint32_t blk_cnt_to_seg_cnt(uint64_t blk_cnt)
{
    return UPPER_BOUND(blk_cnt, BLOCK_PER_SEGMENT);
}

static uint64_t seg_cnt_to_blk_cnt(uint64_t seg_cnt)
{
    return seg_cnt * BLOCK_PER_SEGMENT;
}

static uint64_t blk_idx_to_seg_idx(uint64_t blk_idx)
{
    assert(blk_idx % BLOCK_PER_SEGMENT == 0);
    return blk_idx / BLOCK_PER_SEGMENT;
}

static uint64_t seg_idx_to_blk_idx(uint64_t seg_idx)
{
    return seg_idx * BLOCK_PER_SEGMENT;
}

static int write_block(void *buffer, uint32_t lpa)
{
    return comm_submit_sync_rw_request(&device_env.dev, buffer, lpa * 8, 8, COMM_IO_WRITE);
}

static int read_block(void *buffer, uint32_t lpa)
{
    return comm_submit_sync_rw_request(&device_env.dev, buffer, lpa * 8, 8, COMM_IO_READ);
}

void *wbuffer;  // 用于写入块的buffer
hscfs_super_block *super_buffer;  // 用于保存超级块的缓存

/* 初始化通信层环境 */
int mkfs_init(int argc, char *argv[])
{
    try
    {
        /* 初始化SPDK与通信层使用的环境 */
        HSCFS_LOG(HSCFS_LOG_INFO, "Initializing SPDK and communication layer...");
        std::string trid = parse_trid_from_argv(argc, argv);
        if (trid.empty())
        {
            HSCFS_LOG(HSCFS_LOG_ERROR, "could not find trid of device!");
            return -1;
        }

        if (spdk_init(trid) != 0)
            return -1;
        
        if (device_env_init() != 0)
            return -1;

        /* 初始化会话层并启动会话层轮询线程 */
        if (comm_session_env_init(&device_env.dev) != 0)
            return -1;
        
        wbuffer = comm_alloc_dma_mem(LPA_SIZE);
        if (wbuffer == NULL)
            return -1;
        super_buffer = static_cast<hscfs_super_block*>(comm_alloc_dma_mem(LPA_SIZE));
        if (super_buffer == NULL)
            return -1;

        return 0;
    }
    catch (const std::exception &e)
    {
        HSCFS_LOG(HSCFS_LOG_ERROR, "exception occurred: %s", e.what());
        return -1;
    }
}

/* 获得设备的lpa数目 */
static uint64_t get_lpa_number()
{
    uint64_t sector_num = spdk_nvme_ns_get_num_sectors(device_env.dev.ns);
    uint64_t lpa_num = sector_num / (LPA_SIZE / 512);
    HSCFS_LOG(HSCFS_LOG_INFO, "detected lpa number: %lu.", lpa_num);
    return lpa_num;
}

/* 格式化超级块缓存和SSD超级块，但不写入magic */
static int format_super(uint64_t lpa_num)
{
    if (lpa_num >= UINT32_MAX)
    {
        HSCFS_LOG(HSCFS_LOG_INFO, "SSD size not supported(too large).");
        return -1;
    }

    /* 计算可用block和segment数量 */
    uint32_t segment_count = lpa_num / BLOCK_PER_SEGMENT;  // segment总个数。segment 0从lpa 0开始
    uint64_t block_count = lpa_num - lpa_num % BLOCK_PER_SEGMENT;  // 4KB block总个数，向下对齐到segment
    assert(block_count == segment_count * BLOCK_PER_SEGMENT);
    super_buffer->block_count = block_count;
    super_buffer->segment_count = segment_count;
    super_buffer->segment0_blkaddr = 0;
    HSCFS_LOG(HSCFS_LOG_INFO, "total 4KB block number: %lu, total segment number: %u.", block_count, segment_count);

    /* 计算Meta Journal位置和空间使用量 */
    uint64_t meta_journal_blk_cnt = META_JOURNAL_BLK_CNT;
    uint32_t meta_journal_seg_cnt = blk_cnt_to_seg_cnt(meta_journal_blk_cnt);
    uint32_t meta_journal_start_blk_lpa = BLOCK_PER_SEGMENT;  // Meta Journal紧跟Super Block segment
    super_buffer->meta_journal_blkaddr = meta_journal_start_blk_lpa;
    super_buffer->segment_count_meta_journal = meta_journal_seg_cnt;
    super_buffer->meta_journal_start_blkoff = meta_journal_start_blk_lpa;
    super_buffer->meta_journal_end_blkoff = meta_journal_start_blk_lpa + meta_journal_blk_cnt;
    HSCFS_LOG(HSCFS_LOG_INFO, "Meta Journal start LPA: %u, occupied segment number: %u.", meta_journal_start_blk_lpa, meta_journal_seg_cnt);

    /* 计算SIT表位置和空间使用量 */
    uint64_t sit_blk_cnt = UPPER_BOUND(segment_count, SIT_ENTRY_PER_BLOCK);  // sit表需要的block数目
    uint32_t sit_seg_cnt = blk_cnt_to_seg_cnt(sit_blk_cnt);  // sit表需要的segment数目
    uint32_t sit_start_blk_lpa = seg_idx_to_blk_idx(blk_idx_to_seg_idx(meta_journal_start_blk_lpa) + meta_journal_seg_cnt);  // sit表的起始lpa
    super_buffer->sit_blkaddr = sit_start_blk_lpa;
    super_buffer->segment_count_sit = sit_seg_cnt;
    HSCFS_LOG(HSCFS_LOG_INFO, "SIT start LPA: %u, occupied segment number: %u.", sit_start_blk_lpa, sit_seg_cnt);

    /* 计算NAT表位置和空间使用量 NAT按照可用block全部作为node block来计算使用量 */
    uint64_t nat_blk_cnt = UPPER_BOUND(block_count, NAT_ENTRY_PER_BLOCK);  // nat表需要的block数目
    uint32_t nat_seg_cnt = blk_cnt_to_seg_cnt(nat_blk_cnt);  // nat表需要的segment数目
    uint32_t nat_start_blk_lpa = seg_idx_to_blk_idx(blk_idx_to_seg_idx(sit_start_blk_lpa) + sit_seg_cnt);  // nat表的起始lpa
    super_buffer->nat_blkaddr = nat_start_blk_lpa;
    super_buffer->segment_count_nat = nat_seg_cnt;
    HSCFS_LOG(HSCFS_LOG_INFO, "NAT start LPA: %u, occupied segment number: %u.", nat_start_blk_lpa, nat_seg_cnt);

    /* 计算SRMAP的位置和空间使用量 */
    uint64_t srmap_blk_cnt = UPPER_BOUND(block_count, ENTRIES_IN_SUM);
    uint32_t srmap_seg_cnt = blk_cnt_to_seg_cnt(srmap_blk_cnt);
    uint32_t srmap_start_blk_lpa = seg_idx_to_blk_idx(blk_idx_to_seg_idx(nat_start_blk_lpa) + nat_seg_cnt);
    super_buffer->srmap_blkaddr = srmap_start_blk_lpa;
    super_buffer->segment_count_srmap = srmap_seg_cnt;
    HSCFS_LOG(HSCFS_LOG_INFO, "SRMAP start LPA: %u, occupied segment number: %u.", srmap_start_blk_lpa, srmap_seg_cnt);

    /* 处理main area占用空间 */
    uint32_t ma_start_segid = blk_idx_to_seg_idx(srmap_start_blk_lpa) + srmap_seg_cnt;
    uint32_t ma_start_blk_lpa = seg_idx_to_blk_idx(ma_start_segid);
    if (ma_start_segid >= segment_count)
    {
        HSCFS_LOG(HSCFS_LOG_ERROR, "SSD size not supported(too small).");
        return -1;
    }
    uint32_t ma_seg_cnt = segment_count - ma_start_segid;
    super_buffer->main_blkaddr = ma_start_blk_lpa;
    super_buffer->segment_count_main = ma_seg_cnt;
    HSCFS_LOG(HSCFS_LOG_INFO, "Main Area: start segid = %u, start lpa = %u, occupied segment number = %u.", ma_start_segid, 
        ma_start_blk_lpa, ma_seg_cnt);

    /* 将main area的前两个segment分别作为初始的活跃segment */
    uint32_t cur_segid = ma_start_segid;
    super_buffer->current_node_segment_id = cur_segid++;
    super_buffer->current_data_segment_id = cur_segid++;

    /* 根目录的node block和data block分配在其他函数处理 */
    super_buffer->current_node_segment_blkoff = 1;  // 分配1个node block给root
    super_buffer->current_data_segment_blkoff = 2;  // root的第0级哈希表包含2个数据块

    /* 初始化segment链表头。链表的连接在初始化SIT时进行 */
    super_buffer->first_free_segment_id = cur_segid;
    if (cur_segid >= segment_count)
    {
        HSCFS_LOG(HSCFS_LOG_ERROR, "SSD size not supported(too small).");
        return -1;
    }
    super_buffer->free_segment_count = segment_count - cur_segid;
    super_buffer->first_node_segment_id = INVALID_SEGID;
    super_buffer->first_data_segment_id = INVALID_SEGID;

    /* 初始化根目录inode和空闲nid链表头。nid链表连接将在初始化NAT时进行 */
    super_buffer->root_ino = ROOT_INO;
    super_buffer->next_free_nid = ROOT_INO + 1;

    /* 写入super block */
    if (write_block(super_buffer, 0) != 0)
    {
        HSCFS_LOG(HSCFS_LOG_ERROR, "write super block failed.");
        return -1;
    }

    HSCFS_LOG(HSCFS_LOG_INFO, "format super block complete.");
    return 0;
}

/* 格式化SIT表，应在format_super后调用 */
static int format_sit()
{
    hscfs_sit_block *buffer = static_cast<hscfs_sit_block*>(wbuffer);
    std::memset(buffer, 0, LPA_SIZE);
    uint32_t sit_start_lpa = super_buffer->sit_blkaddr;
    uint32_t sit_segment_cnt = super_buffer->segment_count_sit;
    uint32_t main_start_segid = blk_idx_to_seg_idx(super_buffer->main_blkaddr);
    
    auto get_segid_pos_in_sit = [sit_start_lpa, sit_segment_cnt](uint32_t segid) {
        uint32_t sit_lpa_idx = segid / SIT_ENTRY_PER_BLOCK;
        uint32_t sit_lpa_off = segid % SIT_ENTRY_PER_BLOCK;
        assert(sit_lpa_idx < sit_segment_cnt * BLOCK_PER_SEGMENT);
        return std::make_pair(sit_start_lpa + sit_lpa_idx, sit_lpa_off);
    };

    assert(super_buffer->current_node_segment_id == main_start_segid);
    assert(super_buffer->current_data_segment_id == main_start_segid + 1);
    assert(super_buffer->first_free_segment_id == main_start_segid + 2);

    /* 格式化Main Area中所有segment对应的SIT表项，并建立空闲segment链表 */
    uint32_t sit_lpa, sit_idx;
    uint32_t segid = main_start_segid;
    while (true)
    {
        if (segid == super_buffer->segment_count)  // 全部初始化完成了，将最后一个SIT block buffer写回
        {
            if (write_block(buffer, sit_lpa) != 0)
                goto err_io;
            break;
        }

        std::tie(sit_lpa, sit_idx) = get_segid_pos_in_sit(segid);  // 获取当前segid在SIT表中的位置(lpa和块内sit_entry偏移)
        if (sit_idx == 0)  // 将要写入一个新的SIT块，则将buffer清空
            std::memset(buffer, 0, LPA_SIZE);
        
        hscfs_sit_entry &entry = buffer->entries[sit_idx];

        /* 如果当前segid是活跃segment，则标记被根目录使用的块 */
        if (segid == super_buffer->current_node_segment_id)
        {
            entry.valid_map[0] = 1U;
            entry.vblocks = 1;
        }
        else if (segid == super_buffer->current_data_segment_id)
        {
            entry.valid_map[0] = 3U;
            entry.vblocks = 2;
        }

        /* 否则，它是空闲segment，只需让segment链表指向下一项 */
        else
        {
            uint32_t nxt_seg = segid == super_buffer->segment_count - 1 ? INVALID_SEGID : segid + 1;
            SET_NEXT_SEG(&entry, nxt_seg);
        }

        /* 如果当前表项是当前SIT块的最后一项，则这个块满了，把它写入SSD，接下来buffer将用作下一个SIT块的缓存 */
        if (sit_idx == SIT_ENTRY_PER_BLOCK - 1)
        {
            if (write_block(buffer, sit_lpa) != 0)
                goto err_io;
        }

        segid++;
    }

    HSCFS_LOG(HSCFS_LOG_INFO, "format SIT complete.");
    return 0;

    err_io:
    HSCFS_LOG(HSCFS_LOG_ERROR, "read/write sit block failed.");
    return -1;
}

/* 格式化NAT表，应在format_super后返回 */
static int format_nat()
{
    hscfs_nat_block *buffer = static_cast<hscfs_nat_block*>(wbuffer);
    std::memset(buffer, 0, LPA_SIZE);

    uint32_t nat_start_lpa = super_buffer->nat_blkaddr;
    uint32_t nat_segment_cnt = super_buffer->segment_count_nat;
    uint32_t nat_end_lpa = nat_start_lpa + seg_cnt_to_blk_cnt(nat_segment_cnt);

    auto get_nid_pos_in_nat = [nat_start_lpa, nat_segment_cnt](uint32_t nid) {
        uint32_t nat_lpa_idx = nid / NAT_ENTRY_PER_BLOCK;
        uint32_t nat_lpa_off = nid % NAT_ENTRY_PER_BLOCK;
        return std::make_pair(nat_lpa_idx + nat_start_lpa, nat_lpa_off);
    };

    /* 首先格式化根目录的nat表项 */
    uint32_t root_node_lpa = seg_idx_to_blk_idx(super_buffer->current_node_segment_id);
    buffer->entries[ROOT_INO].ino = ROOT_INO;
    buffer->entries[ROOT_INO].block_addr = root_node_lpa;

    /* 格式化剩余的其他nat表项，建立空闲NAT链表 */
    uint32_t nat_lpa = nat_start_lpa;
    uint32_t nat_idx = ROOT_INO + 1;
    uint32_t cur_nid = ROOT_INO + 1;

    while (nat_lpa < nat_end_lpa)
    {
        auto pos = get_nid_pos_in_nat(cur_nid);
        assert(pos.first == nat_lpa && pos.second == nat_idx);

        uint32_t nxt_nid;
        if (nat_lpa == nat_end_lpa - 1 && nat_idx == NAT_ENTRY_PER_BLOCK - 1)
            nxt_nid = INVALID_NID;  // 如果已经写到NAT表最后一项了（空闲链表尾），置尾部为INVALID_NID
        else
            nxt_nid = cur_nid + 1;
        buffer->entries[nat_idx].ino = 0;
        buffer->entries[nat_idx].block_addr = nxt_nid;

        if (nat_idx == NAT_ENTRY_PER_BLOCK - 1)  // 如果当前NAT块已经写完了，则把他写入SSD，并清空
        {
            if (write_block(buffer, nat_lpa) != 0)
            {
                HSCFS_LOG(HSCFS_LOG_ERROR, "write NAT block failed.");
                return -1;
            }
            std::memset(buffer, 0, LPA_SIZE);
        }

        ++cur_nid;
        ++nat_idx;
        if (nat_idx == NAT_ENTRY_PER_BLOCK)
        {
            nat_idx = 0;
            ++nat_lpa;
        }
    }

    HSCFS_LOG(HSCFS_LOG_INFO, "format NAT complete.");
    return 0;
}

/* 创建main area中根目录的node block和data block */
static int format_main_area()
{
    /* 格式化根目录的索引块 */
    hscfs_node *node = static_cast<hscfs_node*>(wbuffer);
    std::memset(node, 0, LPA_SIZE);

    node->footer.ino = ROOT_INO;
    node->footer.nid = ROOT_INO;
    node->footer.offset = 0;

    hscfs_inode *inode = &node->i;
    inode->i_inline = 0;
    inode->i_type = HSCFS_FT_DIR;
    inode->i_nlink = 1;
    inode->i_size = LPA_SIZE * 2;
    inode->i_dentry_num = 0;
    inode->i_current_depth = 0;
    inode->i_dir_level = 0;

    timespec cur_time;
    timespec_get(&cur_time, TIME_UTC);
    inode->i_atime = cur_time.tv_sec;
    inode->i_atime_nsec = cur_time.tv_nsec;
    inode->i_mtime = cur_time.tv_sec;
    inode->i_mtime_nsec = cur_time.tv_nsec;

    uint32_t first_data_lpa = seg_idx_to_blk_idx(super_buffer->current_data_segment_id);
    inode->i_addr[0] = first_data_lpa;
    inode->i_addr[1] = first_data_lpa + 1;

    if (write_block(node, seg_idx_to_blk_idx(super_buffer->current_node_segment_id)) != 0)
    {
        HSCFS_LOG(HSCFS_LOG_ERROR, "write root node failed.");
        return -1;
    }

    /* 格式化根目录的数据块 */
    std::memset(wbuffer, 0, LPA_SIZE);
    for (int i = 0; i < 2; ++i)
    {
        if (write_block(wbuffer, first_data_lpa + i) != 0)
        {
            HSCFS_LOG(HSCFS_LOG_ERROR, "write root data failed.");
            return -1;
        }
    }

    HSCFS_LOG(HSCFS_LOG_INFO, "format Main Area complete.");
    return 0;
}

/* 初始化SSD DB区域 */
static int init_ssd_db()
{
    comm_dev *dev = &device_env.dev;

    /* SSD侧FS模块初始化 */
    HSCFS_LOG(HSCFS_LOG_INFO, "Initializing SSD fs module...");
    if (comm_submit_fs_module_init_request(dev) != 0)
    {
        HSCFS_LOG(HSCFS_LOG_ERROR, "SSD fs module init failed.");
        return -1;
    }

    /* 
     * 已知缺陷：FS模块初始化是异步的（SSD侧异步读取Super Block），还没有执行完就会向主机侧返回CQE，
     * 执行完后不会通知主机，主机也无法主动轮询该命令执行状态。
     * 所以，后续依赖SSD DRAM中Super Block缓存的命令会执行失败，甚至访问到未初始化的指针。
     * 目前通过主机侧等待足够长的时间来解决此问题
     */
    HSCFS_LOG(HSCFS_LOG_INFO, "Waiting SSD fs module init complete...");
    sleep(3);

    /* 初始化DB区域 */
    HSCFS_LOG(HSCFS_LOG_INFO, "Initializing SSD DB...");
    if (comm_submit_fs_db_init_request(dev) != 0)
    {
        HSCFS_LOG(HSCFS_LOG_ERROR, "SSD DB init failed.");
        return -1;
    }

    /* 清空元数据日志 */
    HSCFS_LOG(HSCFS_LOG_INFO, "Clearing Metajournal FIFO...");
    if (comm_submit_clear_metajournal_request(dev) != 0)
    {
        HSCFS_LOG(HSCFS_LOG_ERROR, "clear Metajournal FIFO failed.");
        return -1;
    }

    return 0;
}

static int write_magic()
{
    HSCFS_LOG(HSCFS_LOG_INFO, "Writing HSCFS magic number...");
    super_buffer->magic = HSCFS_MAGIC_NUMBER;
    if (write_block(super_buffer, 0) != 0)
    {
        HSCFS_LOG(HSCFS_LOG_ERROR, "write magic failed.");
        return -1;
    }
    return 0;
}

/* 将SSD设备格式化为HSCFS */
static int format_ssd()
{
    uint64_t lpa_num = get_lpa_number();
    if (format_super(lpa_num) != 0)
        return -1;
    if (format_sit() != 0)
        return -1;
    if (format_nat() != 0)
        return -1;
    if (format_main_area() != 0)
        return -1;
    if (init_ssd_db() != 0)
        return -1;
    if (write_magic() != 0)
        return -1;
    HSCFS_LOG(HSCFS_LOG_INFO, "HSCFS format complete.");
    return 0;
}

}  // namespace hscfs

int main(int argc, char *argv[])
{
    using namespace hscfs;
    if (mkfs_init(argc, argv) != 0)
        return 1;
    if (format_ssd() != 0)
        return 1;
    comm_free_dma_mem(wbuffer);
    comm_free_dma_mem(super_buffer);
    return 0;
}
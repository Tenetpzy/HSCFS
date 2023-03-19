#pragma once

#include "spdk/nvme.h"
#include "spdk/nvme_spec.h"

#include <list>
#include <vector>
#include <array>
#include <mutex>
#include <condition_variable>
#include <chrono>

enum class SSD_cmd_type
{
    READ, WRITE, VENDOR
};

enum class SSD_cmd_proc_state
{
    NEWLY_FETCHED,  // 刚从SQ中取出
    LONG_CMD_CQE_SENT,  // 对于长命令，发送了CQE，还没处理
    WAITING_MOCK_LATENCY  // 等待模拟时延
};

using time_point_t = std::chrono::time_point<std::chrono::steady_clock>;

class SSD_cmd_info
{
public:
    SSD_cmd_type type;
    union
    {
        // 读写命令
        struct
        {
            uint64_t lba;
            uint32_t lba_count;
        };
        // 自定义命令
        spdk_nvme_cmd vendor;
    };
    uint64_t cid;  // spdk mock分配的命令唯一标识
    void *dma_addr;
    uint32_t dma_len;

private:
    time_point_t start_time;
    SSD_cmd_proc_state state;

    friend class SSD_device_mock;
};

struct SSD_qpair
{
    std::list<SSD_cmd_info> sq;
    std::list<uint64_t> cq;
};

class SSD_device_mock
{
public:
    /*
     * lba_num：模拟SSD的LBA个数
     * io_qpair_size：模拟SSD的I/O qpair个数
     */
    SSD_device_mock(size_t lba_num, size_t io_qpair_size);
    
    void submit_io_cmd(spdk_nvme_qpair *qpair, const SSD_cmd_info &cmd);
    void submit_admin_cmd(const SSD_cmd_info &cmd);
    std::list<uint64_t> get_qpair_cplt_cid_list(spdk_nvme_qpair *qpair, uint32_t max_cplt);
    std::list<uint64_t> get_admin_cplt_cid_list();

    void start_mock();

private:
    using LBA_block = std::array<char, 512>;
    std::vector<LBA_block> flash;

    /*
     * 与上层交互的队列的模拟
     * sq：上层生产，SSD消费，SSD使用sq_cond等待
     * cq：SSD生产，上层通过轮询消费
     */
    std::vector<SSD_qpair> io_qpairs;
    SSD_qpair admin_qpair;
    std::mutex qpair_mtx;  // 保护所有qpair的互斥
    uint64_t total_pending_sq_cmds;  // 所有还未被SSD处理的sq里的命令个数
    std::condition_variable sq_cond; // 对SSD模拟线程进行同步，有命令需要处理时唤醒

    /* SSD主控中的模拟数据 */
    /**********************************/

    /* 
     * 从sq中获取的，因为不满足模拟I/O时延等原因，正在处理或正在等待处理的命令
     */
    std::list<SSD_cmd_info> proc_io_cmds, proc_admin_cmds;
    std::list<SSD_cmd_info> proc_long_cmds;

    

private:
    void mock_thread();  // SSD mock线程入口
    void fetch_new_cmd();  
};


#pragma once

#include "spdk/nvme.h"
#include "spdk/nvme_spec.h"

#include <list>
#include <vector>
#include <array>
#include <mutex>

enum class SSD_cmd_type
{
    READ, WRITE, VENDOR
};

struct SSD_cmd_info
{
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
};

class SSD_qpair
{
public:
    void append_sq(const SSD_cmd_info &cmd)
    {
        sq.emplace_back(cmd);
    }

    

private:
    std::list<SSD_cmd_info> sq;
    std::list<uint64_t> cq;
    std::mutex sq_mtx, cq_mtx;
};

using LBA_block = std::array<char, 512>;

class SSD_device_mock
{
public:
    /*
     * lba_num：模拟SSD的LBA个数
     * channel_size：模拟SSD的I/O qpair个数
     */
    SSD_device_mock(size_t lba_num, size_t channel_size);
    
    spdk_nvme_qpair *alloc_io_qpair();
    void free_io_qpair(spdk_nvme_qpair *qpair);
    void submit_io_cmd(spdk_nvme_qpair *qpair, const SSD_cmd_info &cmd);
    void submit_admin_cmd(const SSD_cmd_info &cmd);
    std::list<uint64_t> get_qpair_cplt_cid_list(spdk_nvme_qpair *qpair, uint32_t max_cplt);
    std::list<uint64_t> get_admin_cplt_cid_list();

private:
    std::vector<LBA_block> flash;
    

private:

};
#pragma once

#include <cstdint>
#include <vector>
#include <memory>
#include <atomic>

#include "utils/declare_utils.hh"
#include "fs/block_buffer.hh"
#include "communication/comm_api.h"

struct comm_dev;

namespace hscfs {

class journal_container;
class journal_output_vector;

class journal_writer
{
public:
    journal_writer(comm_dev *device, uint64_t journal_area_start_lpa, uint64_t journal_area_end_lpa);
    no_copy_assignable(journal_writer)

    // 设置将要处理的日志
    void set_pending_journal(journal_container *journal) noexcept
    {
        cur_journal = journal;
    }

    // 将日志中的日志项收集到写缓存，返回写缓存的块个数
    uint64_t collect_pending_journal_to_write_buffer();

    /* 
     * 将写缓存写入SSD（同步，全部写完后返回）
     * cur_tail：当前日志区域FIFO的队列尾地址
     * 调用者需保证SSD有足够的空间保存本次的日志
     * 
     * 异常：
     * hscfs_io_error：通信层发生错误
     * 其它系统异常
     */
    void write_to_SSD(uint64_t cur_tail);

private:
    std::vector<std::unique_ptr<journal_output_vector>> journal_output_vec_generate() const;
    char *get_ith_buffer_block(size_t index);
    void fill_buffer_with_nop(char *start, char *end);
    void append_end_entry();
    static void async_write_callback(comm_cmd_result res, void *arg);

private:
    journal_container *cur_journal;
    uint64_t start_lpa, end_lpa;  // SSD上日志区域的起止位置[start_lpa, end_lpa)

    std::vector<block_buffer> journal_buffer;  // 4KB缓存块的列表，按需增长
    size_t buffer_tail_idx, buffer_tail_off;  // 当前缓存块列表中，使用到的最后一个缓存块下标和块中偏移

    comm_dev *dev;
};

}  // namespace hscfs
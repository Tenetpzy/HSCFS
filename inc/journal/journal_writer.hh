#pragma once

#include <cstdint>
#include <vector>
#include <memory>

#include "utils/declare_utils.hh"
#include "fs/block_buffer.hh"

class hscfs_journal_container;
class journal_output_vector;

class hscfs_journal_writer
{
public:
    hscfs_journal_writer(uint64_t journal_area_start_lpa, uint64_t journal_area_end_lpa):
        start_lpa(journal_area_start_lpa), end_lpa(journal_area_end_lpa), cur_journal(nullptr),
        buffer_tail_idx(0), buffer_tail_off(0) { }
    no_copy_assignable(hscfs_journal_writer)

    // 设置将要处理的日志
    void set_pending_journal(hscfs_journal_container *journal) noexcept
    {
        cur_journal = journal;
    }

    // 将日志中的日志项收集到写缓存
    void collect_pending_journal_to_write_buffer();

    // 将写缓存以异步方式写入SSD
    void start_write_to_SSD();

private:
    std::vector<std::unique_ptr<journal_output_vector>> journal_output_vec_generate() const;
    char *get_ith_buffer_block(size_t index);
    void fill_buffer_with_nop(char *start, char *end);
    void append_end_entry();

private:
    hscfs_journal_container *cur_journal;
    uint64_t start_lpa, end_lpa;  // SSD上日志区域的起止位置[start_lpa, end_lpa)

    std::vector<hscfs_block_buffer> journal_buffer;  // 4KB缓存块的列表，按需增长
    size_t buffer_tail_idx, buffer_tail_off;  // 当前缓存块列表中，使用到的最后一个缓存块下标和块中偏移
};
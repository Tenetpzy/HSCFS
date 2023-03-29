#pragma once

#include <vector>
#include "journal/journal_type.h"

namespace hscfs {

/*
 * 日志容器，用于存储事务生成的日志项
 */
class journal_container
{
public:
    void append_super_block_journal_entry(const super_block_journal_entry &entry)
    {
        super_block_journal.emplace_back(entry);
    }

    void append_NAT_journal_entry(const NAT_journal_entry &entry)
    {
        NAT_journal.emplace_back(entry);
    }

    void append_SIT_journal_entry(const SIT_journal_entry &entry)
    {
        SIT_journal.emplace_back(entry);
    }

    uint64_t get_tx_id() const noexcept
    {
        return tx_id;
    }

private:
    std::vector<super_block_journal_entry> super_block_journal;
    std::vector<NAT_journal_entry> NAT_journal;
    std::vector<SIT_journal_entry> SIT_journal;

    uint64_t tx_id;  // 事务号

private:
    const std::vector<super_block_journal_entry>& get_super_block_journal() const noexcept
    {
        return super_block_journal;
    }

    const std::vector<NAT_journal_entry>& get_NAT_journal() const noexcept
    {
        return NAT_journal;
    }

    const std::vector<SIT_journal_entry>& get_SIT_journal() const noexcept
    {
        return SIT_journal;
    }

    void set_tx_id(uint64_t id) noexcept
    {
        tx_id = id;
    }

    friend class journal_process_env;
    friend class journal_writer;
    friend class journal_processor;
};

}  // namespace hscfs
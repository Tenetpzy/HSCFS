#include "fs/replace_proc.hh"
#include "journal/journal_container.hh"

namespace hscfs {

transaction_replace_protect_record::transaction_replace_protect_record(
    uint32_t tx_id, 
    std::list<node_block_cache_entry_handle> &&dirty_nodes_,
    std::vector<dentry_handle> &&dirty_dentrys_, 
    std::unique_ptr<journal_container> &tx_journal, 
    std::vector<uint32_t> &&uncommit_node_segs_, 
    std::vector<uint32_t> &&uncommit_data_segs_)
    : dirty_nodes(std::move(dirty_nodes_)), dirty_dentrys(std::move(dirty_dentrys_)), 
    uncommit_node_segs(std::move(uncommit_node_segs_)), uncommit_data_segs(std::move(uncommit_data_segs_))
{
    this->tx_id = tx_id;
    this->tx_journal = std::move(tx_journal);
}

}  // namespace hscfs
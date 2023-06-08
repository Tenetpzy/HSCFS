#include "fs/replace_protect.hh"
#include "fs/server_thread.hh"
#include "fs/fs_manager.hh"
#include "fs/NAT_utils.hh"
#include "fs/SIT_utils.hh"
#include "fs/super_manager.hh"
#include "cache/SIT_NAT_cache.hh"
#include "journal/journal_container.hh"
#include "utils/lock_guards.hh"
#include "utils/hscfs_log.h"
#include <system_error>

namespace hscfs {

transaction_replace_protect_record::transaction_replace_protect_record(
    uint64_t tx_id, 
    std::list<node_block_cache_entry_handle> &&dirty_nodes_,
    std::vector<dentry_handle> &&dirty_dentrys_, 
    std::unique_ptr<journal_container> &&tx_journal_, 
    std::vector<uint32_t> &&uncommit_node_segs_, 
    std::vector<uint32_t> &&uncommit_data_segs_)
    : dirty_nodes(std::move(dirty_nodes_)), dirty_dentrys(std::move(dirty_dentrys_)), tx_journal(std::move(tx_journal_)), 
    uncommit_node_segs(std::move(uncommit_node_segs_)), uncommit_data_segs(std::move(uncommit_data_segs_))
{
    this->tx_id = tx_id;
}

replace_protect_manager::replace_protect_manager(file_system_manager *fs_manager)
{
    this->fs_manager = fs_manager;
    int ret = spin_init(&trp_list_lock);
    if (ret != 0)
        throw std::system_error(std::error_code(ret, std::generic_category()), 
            "replace_protect_manager: init cache trp_list_lock failed.");
}

void replace_protect_manager::notify_cplt_tx(uint64_t cplt_tx_id)
{
    transaction_replace_protect_record first_record;
    {
        spin_lock_guard lg(trp_list_lock);
        assert(trp_list.size() > 0);
        first_record = std::move(trp_list.front());
        trp_list.pop_front();
    }
    
    /* 目前，日志管理层一定是按照提交顺序进行通知 */
    assert(first_record.tx_id == cplt_tx_id);

    /* 构造将该事务的淘汰保护善后任务(增加SSD版本号等)，发送给后台服务线程 */
    server_thread *server = fs_manager->get_server_thread_handle();
    server->post_task(replace_protect_task(fs_manager, std::move(first_record)));
}

void replace_protect_manager::add_tx(transaction_replace_protect_record &&trp)
{
    spin_lock_guard lg(trp_list_lock);
    trp_list.emplace_back(std::move(trp));
}

void replace_protect_task::operator()()
{
    std::lock_guard<std::mutex> fs_meta_lg(fs_manager->get_fs_meta_lock());

    /* 增加SIT/NAT的SSD版本号 */
    auto &journal = cplt_tx->tx_journal;

    SIT_NAT_cache *nat_cache = fs_manager->get_nat_cache();
    nat_lpa_mapping nat_helper(fs_manager);
    for (auto &nat_entry : journal->get_NAT_journal())
    {
        uint32_t nid = nat_entry.nid;
        uint32_t lpa;
        std::tie(lpa, std::ignore) = nat_helper.get_nid_pos_in_nat(nid);
        nat_cache->add_SSD_version(lpa);
        HSCFS_LOG(HSCFS_LOG_DEBUG, "add SSD version of nat cache entry(lpa = %u).", lpa);
    }

    SIT_NAT_cache *sit_cache = fs_manager->get_sit_cache();
    SIT_operator sit_helper(fs_manager);
    for (auto &sit_entry : journal->get_SIT_journal())
    {
        uint32_t segid = sit_entry.segID;
        uint32_t lpa;
        std::tie(lpa, std::ignore) = sit_helper.get_segid_pos_in_sit(segid);
        sit_cache->add_SSD_version(lpa);
        HSCFS_LOG(HSCFS_LOG_DEBUG, "add SSD version of sit cache entry(lpa = %u).", lpa);
    }

    /* 将未提交的segments加入系统对应链表 */
    super_manager *sp_manager = fs_manager->get_super_manager();
    for (auto segid : cplt_tx->uncommit_node_segs)
    {
        sp_manager->add_to_node_segment_list(segid);
        HSCFS_LOG(HSCFS_LOG_DEBUG, "add segid %u to node segment list.");
    }
    for (auto segid : cplt_tx->uncommit_data_segs)
    {
        sp_manager->add_to_data_segment_list(segid);
        HSCFS_LOG(HSCFS_LOG_DEBUG, "add segid %u to data segment list.");
    }

    /* 
     * 必须在持有fs_meta_lock时释放cplt_tx，以析构node_block_cache_entry_handle和dentry_handle，
     * 进而增加它们的SSD版本号（析构时将减少引用计数、访问对应缓存管理器，该过程需要加fs_meta_lock锁）
     */
    assert(cplt_tx.use_count() == 1);
    cplt_tx.reset();
}

} // namespace hscfs
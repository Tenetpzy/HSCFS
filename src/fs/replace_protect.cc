#include "fs/replace_protect.hh"
#include "fs/server_thread.hh"
#include "fs/fs_manager.hh"
#include "fs/NAT_utils.hh"
#include "fs/SIT_utils.hh"
#include "fs/super_manager.hh"
#include "cache/SIT_NAT_cache.hh"
#include "journal/journal_container.hh"
#include "utils/hscfs_log.h"

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
}

void replace_protect_manager::notify_cplt_tx(uint64_t cplt_tx_id)
{
    transaction_replace_protect_record first_record;
    bool is_trp_list_empty;

    {
        std::lock_guard<std::mutex> lg(lock);
        assert(trp_list.size() > 0);
        first_record = std::move(trp_list.front());
        trp_list.pop_front();

        uint64_t tx_id = first_record.tx_id;
        assert(protect_processing_tx.count(tx_id) == 0);
        protect_processing_tx.insert(tx_id);

        is_trp_list_empty = trp_list.empty();
    }
    
    /* 目前，日志管理层一定是按照提交顺序进行通知 */
    assert(first_record.tx_id == cplt_tx_id);

    /* 若trp list为空，通知等待的线程 */
    if (is_trp_list_empty)
        trp_list_empty_cond.notify_all();

    /* 构造将该事务的淘汰保护善后任务(增加SSD版本号等)，发送给后台服务线程 */
    server_thread *server = fs_manager->get_server_thread_handle();
    server->post_task(replace_protect_task(this, std::move(first_record)));
}

void replace_protect_manager::add_tx(transaction_replace_protect_record &&trp)
{
    std::lock_guard<std::mutex> lg(lock);
    trp_list.emplace_back(std::move(trp));
}

void replace_protect_manager::wait_all_protect_task_cplt()
{
    std::unique_lock<std::mutex> lg(lock);

    /* 首先等待所有事务的日志都被SSD应用 */
    trp_list_empty_cond.wait(lg, [&]() {
        return trp_list.empty();
    });

    /* 然后等待所有已应用事务的淘汰保护工作完成 */
    protect_cplt_cond.wait(lg, [&]() {
        return protect_processing_tx.empty();
    });
}

void replace_protect_manager::wait_all_journal_applied_in_SSD()
{
    std::unique_lock<std::mutex> lg(lock);
    trp_list_empty_cond.wait(lg, [&]() {
        return trp_list.empty();
    });
}

void replace_protect_manager::mark_protect_process_cplt(uint64_t tx_id)
{
    bool is_protect_processing_tx_empty;
    {
        std::lock_guard<std::mutex> lg(lock);
        assert(protect_processing_tx.count(tx_id) == 1);
        protect_processing_tx.erase(tx_id);
        is_protect_processing_tx_empty = protect_processing_tx.empty();
    }

    /* 如果所有事务的淘汰保护工作都执行完了，通知等待的线程 */
    if (is_protect_processing_tx_empty)
        protect_cplt_cond.notify_all();
}

replace_protect_task::replace_protect_task(replace_protect_manager *rp_manager_, transaction_replace_protect_record &&cplt_tx_)
{
    this->cplt_tx = std::make_shared<transaction_replace_protect_record>(std::move(cplt_tx_));
    this->rp_manager = rp_manager_;
    this->fs_manager = rp_manager_->fs_manager;
}

void replace_protect_task::operator()()
{
    std::unique_lock<std::mutex> fs_meta_lg(fs_manager->get_fs_meta_lock());

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
    }

    SIT_NAT_cache *sit_cache = fs_manager->get_sit_cache();
    SIT_operator sit_helper(fs_manager);
    for (auto &sit_entry : journal->get_SIT_journal())
    {
        uint32_t segid = sit_entry.segID;
        uint32_t lpa;
        std::tie(lpa, std::ignore) = sit_helper.get_segid_pos_in_sit(segid);
        sit_cache->add_SSD_version(lpa);
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
    uint64_t tx_id = cplt_tx->tx_id;
    assert(cplt_tx.use_count() == 1);
    cplt_tx.reset();

    fs_meta_lg.unlock();  /* 可以先解锁fs_meta_lock了 */

    /* 在replace_protect_manager中标记此事务的淘汰保护工作已经执行完成 */
    rp_manager->mark_protect_process_cplt(tx_id);
}

} // namespace hscfs
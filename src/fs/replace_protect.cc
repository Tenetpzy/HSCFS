#include "fs/replace_protect.hh"
#include "fs/server_thread.hh"
#include "fs/fs_manager.hh"
#include "journal/journal_container.hh"
#include "utils/lock_guards.hh"
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
    server->post_task(std::packaged_task<task_t>(replace_protect_task(fs_manager, std::move(first_record))));
}

void replace_protect_manager::add_tx(transaction_replace_protect_record &&trp)
{
    spin_lock_guard lg(trp_list_lock);
    trp_list.emplace_back(std::move(trp));
}

void replace_protect_task::operator()()
{
    /* TODO : 增加对应缓存项的SSD侧引用计数 */
}

} // namespace hscfs
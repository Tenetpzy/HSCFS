#pragma once

#include <cstdint>
#include <memory>
#include <list>

#include "cache/node_block_cache.hh"
#include "cache/dentry_cache.hh"
#include "utils/hscfs_multithread.h"

namespace hscfs {

class journal_container;
class file_system_manager;

class replace_protect_manager;

/* 一个事务的缓存淘汰保护信息 */
class transaction_replace_protect_record
{
public:
    transaction_replace_protect_record(uint64_t tx_id, std::list<node_block_cache_entry_handle> &&dirty_nodes_,
        std::vector<dentry_handle> &&dirty_dentrys_, std::unique_ptr<journal_container> &&tx_journal_, 
        std::vector<uint32_t> &&uncommit_node_segs_, std::vector<uint32_t> &&uncommit_data_segs_);

private:
    uint64_t tx_id;

    /* 此处相当于对node缓存项和dentry缓存项的主机测版本号+1，不让它们被淘汰(版本号在ref_count中一同维护) */
    std::list<node_block_cache_entry_handle> dirty_nodes;
    std::vector<dentry_handle> dirty_dentrys;

    /* 该事务积累的日志。事务日志在SSD应用后，根据日志中的内容增加SIT和NAT表的SSD侧版本号 */
    std::unique_ptr<journal_container> tx_journal;

    /* 该事务写满的node和data segments。SSD应用完日志后，将它们加入到系统的node和data segments中 */
    std::vector<uint32_t> uncommit_node_segs, uncommit_data_segs;

private:
    transaction_replace_protect_record() {}  /* 只用于在内外层作用域间move */
    friend class replace_protect_manager;
    friend class replace_protect_task;
};

/* 管理系统当前的淘汰保护状态 */
class replace_protect_manager
{
public:
    replace_protect_manager(file_system_manager *fs_manager);

    void notify_cplt_tx(uint64_t cplt_tx_id);

    /* 必须在日志提交前调用 */
    void add_tx(transaction_replace_protect_record &&trp);

private:
    std::list<transaction_replace_protect_record> trp_list;  /* 日志还未应用的事务淘汰保护信息列表 */
    spinlock_t trp_list_lock;  /* 保护上方trp_list的锁 */
    file_system_manager *fs_manager;
};

/* 一个事务日志被SSD应用后，启动此task，增加涉及的缓存项的SSD版本号 */
class replace_protect_task
{
public:
    replace_protect_task(file_system_manager *fs_manager, transaction_replace_protect_record &&cplt_tx_) noexcept
        : cplt_tx(std::move(cplt_tx_))
    {
        this->fs_manager = fs_manager;
    }

    /* task执行的主函数 */
    void operator()();

private:
    transaction_replace_protect_record cplt_tx;
    file_system_manager *fs_manager;
};

}  // namespace hscfs
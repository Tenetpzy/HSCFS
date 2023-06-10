#pragma once

#include <cstdint>
#include <memory>
#include <list>
#include <unordered_set>
#include <mutex>
#include <condition_variable>

#include "cache/node_block_cache.hh"
#include "cache/dentry_cache.hh"
#include "utils/hscfs_multithread.h"
#include "utils/declare_utils.hh"

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

    no_copy_assignable(transaction_replace_protect_record)

    transaction_replace_protect_record(transaction_replace_protect_record &&o) = default;
    transaction_replace_protect_record& operator=(transaction_replace_protect_record &&o) = default;

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
    /* 只用于在内外层作用域间move */
    transaction_replace_protect_record() = default;
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

    /* 
     * 等待所有等待所有已提交，但日志未被SSD应用的事务都被SSD应用，且它们的淘汰保护任务都全部完成
     * 此方法一般在程序即将退出时调用，调用时不得有另一个线程提交日志
     */
    void wait_all_protect_task_cplt();

private:
    
    std::list<transaction_replace_protect_record> trp_list;  /* 日志还未应用的事务淘汰保护信息列表 */

    /* 
     * 用于trp_list为空时进行通知
     * 调用wait_all_protect_task_cplt的线程会等待trp_list为空
     */
    std::condition_variable trp_list_empty_cond;

    /* 
     * 正在进行淘汰保护状态处理的已应用事务id集合
     * 即发送了replace_protect_task，但该task还没有执行完的事务
     */
    std::unordered_set<uint64_t> protect_processing_tx;

    /*
     * 用于protect_processing_tx为空时进行通知
     * 调用wait_all_protect_task_cplt的线程会等待protect_processing_tx为空
     */
    std::condition_variable protect_cplt_cond;

    std::mutex lock;  /* 保护以上数据成员的锁，此锁不能先于fs_meta_lock获取 */

    file_system_manager *fs_manager;

    /* 标记事务tx_id的淘汰保护工作已经处理完了 */
    void mark_protect_process_cplt(uint64_t tx_id);

    friend class replace_protect_task;
};

/* 一个事务日志被SSD应用后，启动此task，增加涉及的缓存项的SSD版本号 */
class replace_protect_task
{
public:
    replace_protect_task(replace_protect_manager *rp_manager_, transaction_replace_protect_record &&cplt_tx_) noexcept
    {
        this->cplt_tx = std::make_shared<transaction_replace_protect_record>(std::move(cplt_tx_));
        this->rp_manager = rp_manager_;
        this->fs_manager = rp_manager_->fs_manager;
    }

    /* 
     * task执行的主函数
     * 内部会加fs_meta_lock锁，文件系统整体回写，等待事务完成时，要注意避免死锁 
     */
    void operator()();

private:

    /* replace_protect_task交给std::function，std::function要求callable可拷贝构造，因此使用指针 */
    std::shared_ptr<transaction_replace_protect_record> cplt_tx;
    replace_protect_manager *rp_manager;
    file_system_manager *fs_manager;
};

}  // namespace hscfs

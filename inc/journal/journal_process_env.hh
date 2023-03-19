#pragma once

#include <list>
#include <mutex>
#include <condition_variable>
#include <atomic>

#include "utils/declare_utils.hh"

class hscfs_journal_container;
struct comm_dev;

class hscfs_journal_process_env
{
public:
    hscfs_journal_process_env() : tx_id_to_alloc(0) { }
    no_copy_assignable(hscfs_journal_process_env)

    // 获取实例之前，必须调用hscfs_journal_module_init初始化。
    static hscfs_journal_process_env* get_instance()
    {
        return &g_env;
    }

    // 提交日志，返回分配的事务号
    uint64_t commit_journal(hscfs_journal_container *journal);

private:
    static hscfs_journal_process_env g_env;

    // 日志管理层的日志提交队列，以及保护该队列的锁，用于通知日志处理线程的条件变量
    std::list<hscfs_journal_container*> commit_queue;
    std::mutex mtx;
    std::condition_variable cond;

    std::atomic<uint64_t> tx_id_to_alloc;

private:

    // 同时超过UNIT64_MAX个事务运行则会分配重复tx_id，暂不考虑这种情况
    uint64_t alloc_tx_id() noexcept
    {
        return atomic_fetch_add(&tx_id_to_alloc, 1UL);
    }

    static void init(comm_dev *dev, uint64_t journal_start_lpa, uint64_t journal_end_lpa, 
        uint64_t journal_fifo_pos);

    friend class hscfs_journal_processor;
};


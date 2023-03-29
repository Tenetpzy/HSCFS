#pragma once

#include <list>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <thread>

#include "utils/declare_utils.hh"

struct comm_dev;

namespace hscfs {

class journal_container;

class journal_process_env
{
public:
    journal_process_env() : tx_id_to_alloc(0) { }
    no_copy_assignable(journal_process_env)
    ~journal_process_env();

    static journal_process_env* get_instance()
    {
        return &g_env;
    }

    // 提交日志，返回分配的事务号
    uint64_t commit_journal(journal_container *journal);

    // 日志处理环境初始化
    void init(comm_dev *dev, uint64_t journal_start_lpa, uint64_t journal_end_lpa, 
        uint64_t journal_fifo_pos);
    
    /* 
     * 向日志处理线程发送停止命令，并等待其停止后返回
     * 日志处理线程在处理完所有已经提交的日志后将会停止
     */
    void stop_process_thread();

private:
    static journal_process_env g_env;

    // 日志管理层的日志提交队列，以及保护该队列的锁，用于通知日志处理线程的条件变量
    std::list<journal_container*> commit_queue;
    bool exit_req = 0;
    std::mutex mtx;
    std::condition_variable cond;

    std::thread process_thread_handle;

    std::atomic<uint64_t> tx_id_to_alloc;

private:

    // 同时超过UNIT64_MAX个事务运行则会分配重复tx_id，暂不考虑这种情况
    uint64_t alloc_tx_id() noexcept
    {
        return tx_id_to_alloc.fetch_add(1, std::memory_order_relaxed);
    }

    friend class journal_processor;
};

}  // namespace hscfs
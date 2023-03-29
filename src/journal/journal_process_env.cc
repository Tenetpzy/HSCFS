#include "journal/journal_process_env.hh"
#include "journal/journal_container.hh"

namespace hscfs {

// 日志处理线程入口
void hscfs_journal_process_thread(comm_dev *dev, uint64_t journal_start_lpa, uint64_t journal_end_lpa, 
    uint64_t journal_fifo_pos);

journal_process_env journal_process_env::g_env;

journal_process_env::~journal_process_env()
{
    if (process_thread_handle.joinable())
        process_thread_handle.join();
}

uint64_t journal_process_env::commit_journal(journal_container *journal)
{
    uint64_t tx_id = alloc_tx_id();
    journal->set_tx_id(tx_id);
    bool need_notify = false;
    {
        std::lock_guard<std::mutex> lg(mtx);
        need_notify = commit_queue.empty();
        commit_queue.push_back(journal);
    }
    if (need_notify)
        cond.notify_all();
    return tx_id;
}

void journal_process_env::init(comm_dev *dev, uint64_t journal_start_lpa, 
    uint64_t journal_end_lpa, uint64_t journal_fifo_pos)
{
    process_thread_handle = std::thread(hscfs_journal_process_thread, dev, journal_start_lpa, 
        journal_end_lpa, journal_fifo_pos);
}

void journal_process_env::stop_process_thread()
{
    {
        std::unique_lock<std::mutex> lg(mtx);
        exit_req = true;
    }
    cond.notify_all();
    process_thread_handle.join();
}

}  // namespace hscfs
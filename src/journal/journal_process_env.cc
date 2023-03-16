#include "journal/journal_process_env.hh"
#include "journal/journal_container.hh"

hscfs_journal_process_env* hscfs_journal_process_env::g_env = nullptr;

uint64_t hscfs_journal_process_env::commit_journal(hscfs_journal_container *journal)
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


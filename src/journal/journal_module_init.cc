#include <thread>

#include "journal/journal_module_init.hh"
#include "journal/journal_processor.hh"
#include "journal/journal_commit_queue.hh"

void hscfs_journal_module_init(uint64_t journal_start_lpa, uint64_t journal_end_lpa, uint64_t journal_fifo_pos)
{
    hscfs_journal_commit_queue::init();
    std::thread journal_process_th(hscfs_journal_process_thread, journal_start_lpa, journal_end_lpa, journal_fifo_pos);
    journal_process_th.detach();
}
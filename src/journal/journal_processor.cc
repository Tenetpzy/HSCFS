#include "journal/journal_processor.hh"
#include "journal/journal_commit_queue.hh"
#include "journal/journal_container.hh"
#include "communication/memory.h"

void hscfs_journal_process_thread(uint64_t journal_start_lpa, uint64_t journal_end_lpa, uint64_t journal_fifo_pos)
{
    hscfs_journal_processor journal_processor(journal_start_lpa, journal_end_lpa, journal_fifo_pos);
    journal_processor.process_journal();
}

hscfs_journal_processor::hscfs_journal_processor(uint64_t journal_start_lpa, uint64_t journal_end_lpa, uint64_t journal_fifo_pos) :
    start_lpa(journal_start_lpa), end_lpa(journal_end_lpa), tail_lpa(journal_fifo_pos)
{
    head_lpa_ptr = static_cast<uint64_t*>(comm_alloc_dma_mem(sizeof(uint64_t)));
    if (head_lpa_ptr == nullptr)
        throw std::bad_alloc();
    
    // 由于SSD使用循环队列维护日志区域，当head_lpa == tail_lpa时视队列为空
    // 所以head_lpa = (tail_lpa + 1) % n 时队列满，n为日志区域块数，时实际可用块要减1
    cur_avail_lpa = total_avail_lpa = end_lpa - start_lpa - 1;

    /* to do */
    /* metajournal 写缓存初始化 */
}

hscfs_journal_processor::~hscfs_journal_processor()
{
    comm_free_dma_mem(head_lpa_ptr);
}

void hscfs_journal_processor::process_journal()
{
    
}

/*
 * 当可用lpa等于整个日志区域时，说明所有已提交的日志均已应用完，不需要再轮询
 * 当journal_list为空时，从journal commit queue中取出的日志均已处理完毕
 * 以上两个条件都满足，则日志处理线程空闲
 */
bool hscfs_journal_processor::is_working() const
{
    return !(cur_avail_lpa == total_avail_lpa && journal_list.empty());
}

void hscfs_journal_processor::fetch_new_journal()
{
    hscfs_journal_commit_queue *journal_queue = hscfs_journal_commit_queue::get_instance();
    std::unique_lock<std::mutex> mtx(journal_queue->mtx, std::defer_lock);  // 此时不锁定mtx

    // 若日志处理线程不空闲，则只尝试获取commit_queue锁，失败，或无新日志，则直接返回
    if (is_working())
    {
        if (!mtx.try_lock())  return;
        if (journal_queue->commit_queue.empty())  return;
    }
    // 若空闲，则睡眠等待commit_queue上有新的日志
    else
    {
        mtx.lock();
        journal_queue->cond.wait(mtx, [journal_queue](){ return !journal_queue->commit_queue.empty(); });
    }

    // 将commit_queue中所有日志按顺序移动到日志处理线程的journal_list尾部
    // 可能的BUG：若STL不保证按原顺序移动，则日志可能被乱序提交
    journal_list.splice(journal_list.end(), journal_queue->commit_queue);

    return;
}


#include "journal/journal_processor.hh"
#include "journal/journal_process_env.hh"
#include "journal/journal_container.hh"
#include "communication/memory.h"
#include "communication/comm_api.h"
#include "utils/hscfs_exceptions.hh"
#include "utils/hscfs_log.h"

// 日志处理线程入口
void hscfs_journal_process_thread(comm_dev *dev, uint64_t journal_start_lpa, uint64_t journal_end_lpa, 
    uint64_t journal_fifo_pos)
{
    hscfs_journal_processor journal_processor(dev, journal_start_lpa, journal_end_lpa, journal_fifo_pos);
    journal_processor.process_journal();
    HSCFS_LOG(HSCFS_LOG_INFO, "journal process thread exit.");
}

hscfs_journal_processor::hscfs_journal_processor(comm_dev *device, uint64_t journal_start_lpa, 
    uint64_t journal_end_lpa, uint64_t journal_fifo_pos): journal_writer(device, journal_start_lpa, journal_end_lpa)
{
    journal_pos_dma_buffer = static_cast<uint64_t*>(comm_alloc_dma_mem(16));
    if (journal_pos_dma_buffer == nullptr) 
        throw hscfs_alloc_error("journal processor: not enough DMA buffer.");

    // 将日志位置查询任务的定时器设置为阻塞式，到达轮询周期后，日志处理线程被唤醒并进行查询任务
    if (hscfs_timer_constructor(&journal_poll_timer, 1) != 0)
        throw hscfs_timer_error("journal processor: init timer failed.");
    // 日志位置查询任务，周期100us
    timespec journal_poll_time = {.tv_sec = 0, .tv_nsec = 100 * 1000};
    // 定时器设置为周期定时器
    hscfs_timer_set(&journal_poll_timer, &journal_poll_time, 1);
    is_poll_timer_enabled = false;

    // 由于SSD使用循环队列维护日志区域，当head_lpa == tail_lpa时视队列为空
    // 所以head_lpa = (tail_lpa + 1) % n 时队列满，n为日志区域块数，实际可用块要减1
    cur_avail_lpa = total_avail_lpa = journal_end_lpa - journal_start_lpa - 1;

    dev = device;
    head_lpa = tail_lpa = journal_fifo_pos;
    start_lpa = journal_start_lpa;
    end_lpa = journal_end_lpa;
    cur_journal = nullptr;
}

hscfs_journal_processor::~hscfs_journal_processor()
{
    comm_free_dma_mem(journal_pos_dma_buffer);
    hscfs_timer_stop(&journal_poll_timer);
    hscfs_timer_destructor(&journal_poll_timer);
}

void hscfs_journal_processor::process_journal()
{
    while (true)
    {
        try {
            fetch_new_journal();
        }
        catch (thread_interrupted &e) {
            break;
        }
        process_pending_journal();
        process_cplt_journal();
    }
}

/*
 * 当可用lpa等于整个日志区域时，说明所有已提交的日志均已应用完，不需要再轮询
 * 当journal_list和cur_journal都为空时，从journal commit queue中取出的日志均已处理完毕
 * 以上两个条件都满足，则日志处理线程空闲
 */
bool hscfs_journal_processor::is_working() const
{
    return !(cur_avail_lpa == total_avail_lpa && pending_journal_list.empty() && cur_journal == nullptr);
}

void hscfs_journal_processor::fetch_new_journal()
{
    hscfs_journal_process_env *process_env = hscfs_journal_process_env::get_instance();
    auto check_if_interrupted = [process_env]() {
        if (process_env->exit_req)
            throw thread_interrupted();
    };
    std::unique_lock<std::mutex> mtx(process_env->mtx);

    check_if_interrupted();

    if (is_working())
    {
        if (process_env->commit_queue.empty())  
            return;
    }
    else
    {
        process_env->cond.wait(mtx, [process_env, &check_if_interrupted](){ 
            check_if_interrupted();
            return !process_env->commit_queue.empty(); 
        });
    }

    // 将commit_queue中所有日志按顺序移动到日志处理线程的journal_list尾部
    // 可能的BUG：若STL不保证按原顺序移动，则日志可能被乱序提交
    pending_journal_list.splice(pending_journal_list.end(), process_env->commit_queue);
    return;
}

void hscfs_journal_processor::process_pending_journal()
{
    if (cur_journal == nullptr)
    {
        if (!pending_journal_list.empty())
        {
            cur_journal = pending_journal_list.front();
            pending_journal_list.pop_front();
            cur_proc_state = journal_process_state::NEWLY_FETCHED;
        }
        else
            return;
    }

    switch (cur_proc_state)
    {
    case journal_process_state::NEWLY_FETCHED:
        write_journal_to_buffer();
        break;

    case journal_process_state::WRITTEN_IN_BUFFER:
        if (write_journal_to_SSD() == true)
        {
            generate_tx_record();
            cur_journal = nullptr;
        }
        break;
    }
}

void hscfs_journal_processor::write_journal_to_buffer()
{
    journal_writer.set_pending_journal(cur_journal);
    cur_journal_block_num = journal_writer.collect_pending_journal_to_write_buffer();
    cur_proc_state = journal_process_state::WRITTEN_IN_BUFFER;
}

bool hscfs_journal_processor::write_journal_to_SSD()
{
    if (cur_journal_block_num <= cur_avail_lpa)
    {
        journal_writer.write_to_SSD(tail_lpa);
        int ret = comm_submit_sync_update_metajournal_tail_request(dev, tail_lpa, cur_journal_block_num);
        if (ret != 0)
            throw hscfs_io_error("journal processor: update SSD journal tail failed.");

        cur_journal_start_lpa = tail_lpa;
        tail_lpa += cur_journal_block_num;
        if (tail_lpa >= end_lpa)
            tail_lpa = tail_lpa - end_lpa + start_lpa;
        cur_journal_end_lpa = tail_lpa;
        cur_avail_lpa -= cur_journal_block_num;
        return true;
    }
    else
        return false;
}

void hscfs_journal_processor::generate_tx_record()
{
    tx_record.emplace_back(cur_journal->get_tx_id(), cur_journal_start_lpa, cur_journal_end_lpa);
}

void hscfs_journal_processor::process_cplt_journal()
{
    if (cur_avail_lpa == total_avail_lpa)
    {
        disable_poll_timer();
        return;
    }
    enable_poll_timer();
    wait_poll_timer();

    // 如果SSD应用了一部分日志，则确认哪些事务日志已经应用完，并做相应处理
    if (sync_with_SSD_journal_pos())
        process_tx_record();
}

void hscfs_journal_processor::enable_poll_timer()
{
    if (is_poll_timer_enabled)
        return;
    if (hscfs_timer_start(&journal_poll_timer) != 0)
        throw hscfs_timer_error("journal porcessor: enable timer failed.");
    is_poll_timer_enabled = true;
}

void hscfs_journal_processor::disable_poll_timer()
{
    if (!is_poll_timer_enabled)
        return;
    if (hscfs_timer_stop(&journal_poll_timer) != 0)
        throw hscfs_timer_error("jounal processor: disable timer failed.");
    is_poll_timer_enabled = false;
}

void hscfs_journal_processor::wait_poll_timer()
{
    if (hscfs_timer_check_expire(&journal_poll_timer, NULL) != 0)
        throw hscfs_timer_error("jounal processor: wait timer failed.");
}

bool hscfs_journal_processor::sync_with_SSD_journal_pos()
{
    if (comm_submit_sync_get_metajournal_head_request(dev, journal_pos_dma_buffer) != 0)
        throw hscfs_io_error("journal processor: submit get journal pos failed.");
    uint64_t new_head_lpa = journal_pos_dma_buffer[0];
    uint64_t new_avail_lpa = new_head_lpa >= head_lpa ? new_head_lpa - head_lpa :
        new_head_lpa + end_lpa - start_lpa - head_lpa;
    head_lpa = new_head_lpa;
    cur_avail_lpa += new_avail_lpa;
    return new_avail_lpa;
}

void hscfs_journal_processor::process_tx_record()
{
    while (true)
    {
        if (tx_record.empty())
            break;
        auto &tx_rc = tx_record.front();
        if (tx_rc.is_applied(head_lpa, tail_lpa))
        {
            /* to do */
            /* 调用淘汰保护模块，通知该事务日志已经应用完成 */
            tx_record.pop_front();
        }
        else
            break;
    }
}

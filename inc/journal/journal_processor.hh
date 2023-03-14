#pragma once

#include <cstdint>
#include <list>

#include "utils/declare_utils.hh"

class hscfs_journal_container;

// 事务日志记录，一个对象代表一个事务，记录该事务日志在SSD上持久化的LPA范围[start_lpa, end_lpa)
class hscfs_transaction_journal_record
{
public:
    hscfs_transaction_journal_record(uint64_t tx_id, uint64_t start_lpa, uint64_t end_lpa) :
        _tx_id(tx_id), _start_lpa(start_lpa), _end_lpa(end_lpa) { }

    uint64_t get_tx_id() const noexcept { return _tx_id; }
    uint64_t get_start_lpa() const noexcept { return _start_lpa; }
    uint64_t get_end_lpa() const noexcept { return _end_lpa; }

    /*
     * 日志处理线程中，只有在事务日志记录移除后，才释放该事务占用的日志区域资源，增加cur_avail_lpa
     * 所以，在此事务日志记录未移除时，cur_head不会二次进入[_start_lpa, _end_lpa)区域
     * 所以可以用如下方法判断日志是否已经应用完
     */
    bool is_applied(uint64_t cur_head) const noexcept
    {
        if (_start_lpa < _end_lpa)
            return cur_head >= _end_lpa || cur_head < _start_lpa;
        else
            return cur_head >= _end_lpa && cur_head < _start_lpa;
    }

private:
    uint64_t _tx_id, _start_lpa, _end_lpa;
};

/* 
 * 日志处理线程环境
 * 系统确保此环境构造时，已完成故障恢复
 * 因此可用日志资源为整个SSD日志区域
 */
class hscfs_journal_processor
{
public:
    hscfs_journal_processor(uint64_t journal_start_lpa, uint64_t journal_end_lpa, uint64_t journal_fifo_pos);
    no_copy_assignable(hscfs_journal_processor)
    ~hscfs_journal_processor();

    void process_journal();

private:
    /*
     * SSD上日志区域的起止LPA：[start_lpa, end_lpa)
     * 当前日志区域FIFO的首尾LPA：[head_lpa, tail_lpa)
     * head_lpa需要DMA获取
     */
    uint64_t start_lpa, end_lpa, tail_lpa;
    uint64_t *head_lpa_ptr;

    // 当前可用lpa数量与总共可用lpa数量
    uint64_t cur_avail_lpa, total_avail_lpa;

    // 日志提交列表，从journal_commit_queue中取到此处
    std::list<hscfs_journal_container*> journal_list;

    /*
     * 事务记录表
     * 确定首事务日志已经应用完毕后，移除它并通知淘汰保护模块
     * 表中事务按提交顺序排列，提交时按顺序写入SSD的Journal FIFO，所以也一定按顺序被应用
     */
    std::list<hscfs_transaction_journal_record> tx_record;

private:

    bool is_working() const;

    // 当日志处理线程空闲时，睡眠等待新的日志，否则尝试获取新的日志
    void fetch_new_journal();
    
    void process_journal_list_head();
};

// 日志处理线程入口
void hscfs_journal_process_thread(uint64_t journal_start_lpa, uint64_t journal_end_lpa, uint64_t journal_fifo_pos);
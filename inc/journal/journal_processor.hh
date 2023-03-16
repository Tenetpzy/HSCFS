#pragma once

#include <cstdint>
#include <list>

#include "journal/journal_writer.hh"
#include "utils/declare_utils.hh"
#include "utils/hscfs_timer.h"

class hscfs_journal_container;
struct comm_dev;

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
     * 所以，在此事务日志记录未移除时，cur_tail不会二次进入[_start_lpa, _end_lpa)区域
     * 所以可以用如下方法判断日志是否已经应用完
     */
    bool is_applied(uint64_t cur_head, uint64_t cur_tail) const noexcept
    {
        if (_start_lpa < _end_lpa)
        {
            if (cur_head <= cur_tail)
                return cur_head >= _end_lpa || cur_tail < _start_lpa;
            else
                return cur_head >= _end_lpa;
        }
        else
        {
            return cur_head <= cur_tail && cur_head >= _end_lpa;
        }
    }

private:
    uint64_t _tx_id, _start_lpa, _end_lpa;
};

/* 
 * 日志处理线程与其工作环境
 * 系统确保此环境构造时，已完成故障恢复，因此可用日志资源为整个SSD日志区域
 * 
 * 日志处理线程中，写日志、写日志尾指针、查询SSD日志位置，都使用同步阻塞式I/O
 * 
 * to do : 设置日志处理线程的退出机制
 */
class hscfs_journal_processor
{
public:
    hscfs_journal_processor(comm_dev *device, uint64_t journal_start_lpa, uint64_t journal_end_lpa, 
        uint64_t journal_fifo_pos);
    no_copy_assignable(hscfs_journal_processor)
    ~hscfs_journal_processor();

    // 日志处理线程的入口
    void process_journal();

private:
    comm_dev *dev;
    uint64_t head_lpa, tail_lpa;  // 当前日志区域FIFO的首尾LPA：[head_lpa, tail_lpa)
    uint64_t start_lpa, end_lpa;  // SSD日志区域起止LPA：[start_lpa, end_lpa)
    uint64_t *journal_pos_dma_buffer;  // 从SSD获取日志头尾指针的DMA缓存区
    uint64_t cur_avail_lpa, total_avail_lpa;  // 当前可用lpa数量与总共可用lpa数量
    
    std::list<hscfs_journal_container*> pending_journal_list;  // 日志提交列表，从journal_process_env中取到此处

    /*
     * 事务记录表
     * 确定首事务日志已经应用完毕后，移除它并通知淘汰保护模块
     * 表中事务按提交顺序排列，提交时按顺序写入SSD的Journal FIFO，所以也一定按顺序被应用
     */
    std::list<hscfs_transaction_journal_record> tx_record;

    hscfs_journal_writer journal_writer;

    // 当前日志记录处理状态
    enum class journal_process_state
    {
        NEWLY_FETCHED,  // 获取到，但还没写入buffer
        WRITTEN_IN_BUFFER,  // 写入buffer，但还没写入SSD(可能由于SSD侧日志空间不足)
    };
    uint64_t cur_journal_block_num;  // 当前日志记录占用的SSD block数目
    hscfs_journal_container *cur_journal;  // 当前正在处理的日志记录
    journal_process_state cur_proc_state;  // 当前日志记录处理状态
    uint64_t cur_journal_start_lpa, cur_journal_end_lpa;  // 当前日志记录的持久化区域

    hscfs_timer journal_poll_timer;   // 控制日志位置查询的定时器
    bool is_poll_timer_enabled;

private:

    // 判断日志处理线程是否空闲
    bool is_working() const;

    // 当日志处理线程空闲时，睡眠等待新的日志，否则尝试获取新的日志
    void fetch_new_journal();
    
    // 处理位于提交队列首部的日志
    void process_pending_journal();

    // 使用journal_writer把当前处理的日志收集到写缓存
    void write_journal_to_buffer();

    /* 
     * 尝试将日志记录写入SSD，若因空间不足无法写，返回false，否则返回true
     * 同时更新SSD尾指针和tail_lpa、cur_avail_lpa成员
     * 此操作是同步操作，阻塞到全部写完后再返回
     */
    bool write_journal_to_SSD();

    // 生成本次处理的日志的事务记录
    void generate_tx_record();

    // 轮询并处理SSD已应用的日志记录
    void process_cplt_journal();

    void enable_poll_timer();
    void disable_poll_timer();
    void wait_poll_timer();

    /* 查询SSD侧日志头尾指针位置，同时更新head_lpa、cur_avail_lpa成员
     * 如果SSD侧应用了一部分日志，则返回true，否则返回false
     */
    bool sync_with_SSD_journal_pos();

    /* 
     * 处理日志已经完成应用的事务
     * 将所有已完成应用的事务移出tx_record链表，并通知淘汰保护模块
     */
    void process_tx_record();
};

// 日志处理线程入口
void hscfs_journal_process_thread(comm_dev *dev, uint64_t journal_start_lpa, uint64_t journal_end_lpa, 
    uint64_t journal_fifo_pos);
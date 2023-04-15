/* ************************************************

注意：
（已修复）日志层测试时，一次只能运行一个测试用例，必须使用--gtest_filter指定目标用例，否则可能segment fault.

原因：
目前日志层环境是全局变量，且退出请求exit_flag一旦置位，不会被复位，
可能导致后续新启动的日志线程拿到置位的exit_flag，直接退出，但测试线程仍写入了日志，然后等待日志线程退出，
此时，早已退出的日志线程没有处理该日志。等到下一测试用例拿到上一测试用例写入的日志时，该日志所在内存已被释放。

已修复该问题，一次可运行全部测试用例，但会导致事务号语义上的错误（所有测试用例共享全局事务号）。

************************************************* */

#include "journal/journal_container.hh"
#include "journal/journal_process_env.hh"
#include "communication/comm_api.h"
#include "communication/dev.h"
#include "cache/block_buffer.hh"
#include "fmt/ostream.h"
#include "gtest/gtest.h"

#include <vector>
#include <stdexcept>
#include <cstring>
#include <cstdlib>
#include <string>
#include <iostream>
#include <unordered_map>
#include <memory>

extern "C" {

struct spdk_nvme_ctrlr{
    int unused;
};
struct spdk_nvme_ns{
    int unused;
};

void *spdk_zmalloc(size_t size, size_t align, uint64_t *phys_addr, int socket_id, uint32_t flags)
{
    return malloc(size);
}

void spdk_free(void *buf)
{
    free(buf);
}

}

using namespace hscfs;

struct journal_area_mock
{
    std::vector<block_buffer> flash;
    uint64_t start_lpa, end_lpa, head_lpa, tail_lpa;
} journal_area;

comm_dev dev;

namespace hscfs {
void print_journal_block(const char *start);
}

static void print_block(uint64_t lpa)
{
    print_journal_block(journal_area.flash[lpa].get_ptr());
}

int comm_submit_async_rw_request(comm_dev *dev, void *buffer, uint64_t lba, uint32_t lba_count,
    comm_async_cb_func cb_func, void *cb_arg, comm_io_direction dir)
{
    if ((lba & 7) != 0)
        std::cerr << "read write mock: unaligned lba.\n";
    for (size_t i = 0; i < lba_count; ++i)
    {
        uint64_t tar_lba = lba + i;
        uint64_t tar_lpa = tar_lba >> 3;
        uint64_t lpa_off = (tar_lba & 7) * 512;
        char *p1 = journal_area.flash[tar_lpa].get_ptr() + lpa_off;
        char *p2 = static_cast<char*>(buffer) + i * 512;
        if (dir == COMM_IO_READ)
            std::memcpy(p2, p1, 512);
        else
            std::memcpy(p1, p2, 512);
    }
    cb_func(COMM_CMD_SUCCESS, cb_arg);
    return 0;
}

int comm_submit_sync_update_metajournal_tail_request(comm_dev *dev, uint64_t origin_lpa, uint32_t write_block_num)
{
    if (origin_lpa != journal_area.tail_lpa)
        throw std::logic_error("origin lpa does not equal to journal tail lpa.");
    
    // write_block_num check
    uint64_t num_can_write;
    uint64_t total_lpa_num = journal_area.end_lpa - journal_area.start_lpa;
    if (journal_area.tail_lpa >= journal_area.head_lpa)
        num_can_write = total_lpa_num - (journal_area.tail_lpa - journal_area.head_lpa) - 1;
    else
        num_can_write = journal_area.head_lpa - journal_area.tail_lpa - 1;
    if (write_block_num > num_can_write)
    {
        fmt::println(std::cerr, "write block num = {}, num can write = {}", write_block_num, num_can_write);
        throw std::logic_error("write block num is larger than num can write.");
    }
    
    journal_area.tail_lpa += write_block_num;
    if (journal_area.tail_lpa >= journal_area.end_lpa)
        journal_area.tail_lpa = journal_area.tail_lpa - journal_area.end_lpa + journal_area.start_lpa;
    
    return 0;
}

/* 
 * mock获取头指针，将头指针推进upper(unapplied_lpa_num / 2)，并print出推进的日志内容
 */
int comm_submit_sync_get_metajournal_head_request(comm_dev *dev, uint64_t *head_lpa)
{
    auto get_unapplied_lpa_num = [](const journal_area_mock &journal)
    {
        if (journal.tail_lpa >= journal.head_lpa)
            return journal.tail_lpa - journal.head_lpa;
        else
            return journal.tail_lpa + journal.end_lpa - journal.start_lpa - journal.head_lpa; 
    };

    uint64_t unapplied = get_unapplied_lpa_num(journal_area);
    if (unapplied == 0)
    {
        *head_lpa = journal_area.head_lpa;
        return 0;
    }

    uint64_t new_head = journal_area.head_lpa + (unapplied + 1) / 2;
    if (new_head >= journal_area.end_lpa)
        new_head = new_head - journal_area.end_lpa + journal_area.start_lpa;
    
    uint64_t &cur_head = journal_area.head_lpa;
    while (cur_head != new_head)
    {
        if (cur_head == journal_area.end_lpa)
        {
            cur_head = journal_area.start_lpa;
            continue;
        }
        fmt::println(std::cout, "journal in LPA {}:", cur_head);
        print_block(cur_head);
        std::cout << std::endl;
        ++cur_head;
    }

    *head_lpa = cur_head;
    return 0;
}

// 不用TEST_F，考虑到每个测试用例可能使用不同的日志范围，不能在程序开始时就指定
class journal_test_env
{
public:
    journal_test_env(uint64_t start, uint64_t end, uint64_t fifo_pos)
    {
        journal_area.start_lpa = start;
        journal_area.end_lpa = end;
        journal_area.head_lpa = journal_area.tail_lpa = fifo_pos;

        journal_area.flash.clear();
        journal_area.flash.resize(end); 
    }
};

void generate_random_journal(journal_container *journal, size_t super_num, size_t nat_num, size_t sit_num)
{
    std::srand(time(nullptr));
    for (size_t i = 0; i < sit_num; ++i)
    {
        SIT_journal_entry entry = {.segID = static_cast<uint32_t>(std::rand())};
        journal->append_SIT_journal_entry(entry);
    }
    for (size_t i = 0; i < nat_num; ++i)
    {
        NAT_journal_entry entry = {.nid = static_cast<uint32_t>(std::rand())};
        journal->append_NAT_journal_entry(entry);
    }
    for (size_t i = 0; i < super_num; ++i)
    {
        super_block_journal_entry entry = {.Off = static_cast<uint32_t>(std::rand())};
        journal->append_super_block_journal_entry(entry);
    }
}

/* 
 * 测试基本的写入功能
 * 在一个块中写入少许日志
 */
TEST(journal, write_in_one_block)
{
    uint64_t start_lpa = 1, end_lpa = 5, fifo_init = 1;
    journal_test_env test_env(start_lpa, end_lpa, fifo_init);
    auto proc_env = journal_process_env::get_instance();
    proc_env->init(&dev, start_lpa, end_lpa, fifo_init);

    journal_container journal;
    for (uint32_t i = 0; i < 5; ++i)
    {
        super_block_journal_entry super_entry = {.Off = i, .newVal = 0};
        NAT_journal_entry nat_entry = {.nid = 4 - i};
        SIT_journal_entry sit_entry = {.segID = 4 - i};
        journal.append_super_block_journal_entry(super_entry);
        journal.append_NAT_journal_entry(nat_entry);
        journal.append_SIT_journal_entry(sit_entry);
    }
    proc_env->commit_journal(&journal);
    proc_env->stop_process_thread();
}

// 测试跨块写入
TEST(journal, write_across_block)
{
    uint64_t start_lpa = 1, end_lpa = 10, fifo_init = 1;
    journal_test_env test_env(start_lpa, end_lpa, fifo_init);
    auto proc_env = journal_process_env::get_instance();
    proc_env->init(&dev, start_lpa, end_lpa, fifo_init);

    const size_t siz = 4096 + 512;
    journal_container journal;

    const size_t sit_write_num = siz / sizeof(SIT_journal_entry);
    fmt::println(std::cout, "generate sit item num: {}", sit_write_num);
    const size_t nat_write_num = siz / sizeof(NAT_journal_entry);
    fmt::println(std::cout, "generate nat item num: {}", nat_write_num);
    const size_t super_write_num = siz / sizeof(super_block_journal_entry);
    fmt::println(std::cout, "generate super item num: {}", super_write_num);
    
    generate_random_journal(&journal, super_write_num, nat_write_num, sit_write_num);

    proc_env->commit_journal(&journal);
    proc_env->stop_process_thread();
}

// 跨块多事务写入
TEST(journal, cross_block_and_multi_tx)
{
    uint64_t start_lpa = 1, end_lpa = 15, fifo_init = 1;
    journal_test_env test_env(start_lpa, end_lpa, fifo_init);
    auto proc_env = journal_process_env::get_instance();
    proc_env->init(&dev, start_lpa, end_lpa, fifo_init);

    const size_t siz = 4096 + 512;

    const size_t sit_write_num = siz / sizeof(SIT_journal_entry);
    fmt::println(std::cout, "generate sit item num: {}", sit_write_num);
    const size_t nat_write_num = siz / sizeof(NAT_journal_entry);
    fmt::println(std::cout, "generate nat item num: {}", nat_write_num);
    const size_t super_write_num = siz / sizeof(super_block_journal_entry);
    fmt::println(std::cout, "generate super item num: {}", super_write_num);

    const size_t tx_num = 2;
    journal_container journal[tx_num];;

    for (size_t i = 0; i < tx_num; ++i)
    {
        generate_random_journal(&journal[i], super_write_num, nat_write_num, sit_write_num);
        fmt::println(std::cerr, "transaction {} commit journal.", i);
        proc_env->commit_journal(&journal[i]);
    }

    proc_env->stop_process_thread();
}

/*
 * 边界条件测试1
 * 一个块内刚好写到只剩4字节
 */
TEST(journal, boundary1)
{
    uint64_t start_lpa = 1, end_lpa = 5, fifo_init = 1;
    journal_test_env test_env(start_lpa, end_lpa, fifo_init);
    auto proc_env = journal_process_env::get_instance();
    proc_env->init(&dev, start_lpa, end_lpa, fifo_init);

    journal_container journal;
    const size_t super_num = 4092 / sizeof(super_block_journal_entry);
    for (uint32_t i = 0; i < super_num; ++i)
    {
        super_block_journal_entry super_entry = {.Off = i, .newVal = 0};
        journal.append_super_block_journal_entry(super_entry);
    }
    proc_env->commit_journal(&journal);
    proc_env->stop_process_thread();
}

/*
 * 边界条件测试2
 * 一个块内写下所有日志项，刚好写满
 */
TEST(journal, boundary2)
{
    uint64_t start_lpa = 1, end_lpa = 5, fifo_init = 1;
    journal_test_env test_env(start_lpa, end_lpa, fifo_init);
    auto proc_env = journal_process_env::get_instance();
    proc_env->init(&dev, start_lpa, end_lpa, fifo_init);

    journal_container journal;
    // 340个NAT条目+1个Super条目，刚好4096
    for (uint32_t i = 0; i < 340; ++i)
    {
        NAT_journal_entry entry = {.nid = i};
        journal.append_NAT_journal_entry(entry);
    }
    super_block_journal_entry sp_entry = {.Off = 0};
    journal.append_super_block_journal_entry(sp_entry);
    proc_env->commit_journal(&journal);
    proc_env->stop_process_thread();
}

/*
 * 边界条件测试3
 * 日志指针回转测试
 * 循环队列中头尾指针回到队列首的情况
 */
TEST(journal, boundary3)
{
    uint64_t start_lpa = 0, end_lpa = 10, fifo_init = 9;
    journal_test_env test_env(start_lpa, end_lpa, fifo_init);
    auto proc_env = journal_process_env::get_instance();
    proc_env->init(&dev, start_lpa, end_lpa, fifo_init);

    const size_t siz = 4096;
    journal_container journal;
    generate_random_journal(&journal, siz / sizeof(super_block_journal_entry), 
        siz / sizeof(NAT_journal_entry), siz / sizeof(SIT_journal_entry));
    proc_env->commit_journal(&journal);

    proc_env->stop_process_thread();

    fmt::println(std::cerr, "after write: head = {}, tail = {}", journal_area.head_lpa, journal_area.tail_lpa);
}

/*
 * 边界条件测试4
 * 日志处理线程等待SSD有可用空间
 */
TEST(journal, boundary4)
{
    uint64_t start_lpa = 0, end_lpa = 4, fifo_init = 1;
    journal_test_env test_env(start_lpa, end_lpa, fifo_init);
    auto proc_env = journal_process_env::get_instance();
    proc_env->init(&dev, start_lpa, end_lpa, fifo_init);

    const size_t siz = 4096;
    const size_t tx_num = 2;
    journal_container journal[tx_num];;

    for (size_t i = 0; i < tx_num; ++i)
    {
        generate_random_journal(&journal[i], siz / sizeof(super_block_journal_entry), 
            siz / sizeof(NAT_journal_entry), siz / sizeof(SIT_journal_entry));
        fmt::println(std::cerr, "transaction {} committed journal.", i);
        proc_env->commit_journal(&journal[i]);
    }

    proc_env->stop_process_thread();
    fmt::println(std::cerr, "after write: head = {}, tail = {}", journal_area.head_lpa, journal_area.tail_lpa);
}

int main(int argc, char **argv)
{
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
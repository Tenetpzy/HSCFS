#include "fs/fs_manager.hh"
#include "fs/fs.h"
#include "fs/path_utils.hh"
#include "fs/server_thread.hh"
#include "cache/super_cache.hh"
#include "journal/journal_process_env.hh"
#include "communication/comm_api.h"
#include "fmt/ostream.h"
#include <mutex>
#include <string>
#include <stdexcept>
#include <unordered_map>
#include <cassert>
#include <iostream>

#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

using namespace hscfs;

struct journal_area_mock
{
    uint64_t start_lpa, end_lpa, head_lpa, tail_lpa;
} journal_area;

int fd;
std::mutex rw_mtx;

#define LBA_SIZE 512

static void host_mock_init()
{
    fd = open("./fsImage", O_RDWR);
    if (fd == -1)
        throw std::runtime_error("open fsImage error");
}

void host_test_env_setup()
{
    host_mock_init();

    file_system_manager::init(nullptr);
    file_system_manager *fs_manager = file_system_manager::get_instance();
    super_cache &super = *(fs_manager->get_super_cache());
    
    journal_process_env::get_instance()->init(nullptr, super->meta_journal_start_blkoff, 
        super->meta_journal_end_blkoff, super->meta_journal_start_blkoff);
    
    journal_area.start_lpa = super->meta_journal_start_blkoff;
    journal_area.end_lpa = super->meta_journal_end_blkoff;
    journal_area.head_lpa = journal_area.tail_lpa = super->meta_journal_start_blkoff;
}

void host_test_env_teardown()
{
    file_system_manager::get_instance()->fini();
    journal_process_env::get_instance()->stop_process_thread();
    close(fd);
}

void do_exit(const char *msg)
{
    perror(msg);
    host_test_env_teardown();
    exit(1);
}

static void do_pwrite(int fd, void *buffer, size_t count, off_t offset)
{
    if (pwrite(fd, buffer, count, offset) < ssize_t(count))
        throw std::runtime_error("write error");
}

static void do_pread(int fd, void *buffer, size_t count, off_t offset)
{
    if (pread(fd, buffer, count, offset) < ssize_t(count))
        throw std::runtime_error("read error");
}

namespace hscfs {
void print_journal_block(const char *start);

int init(int argc, char *argv[])
{
    host_test_env_setup();
    return 0;
}

void fini()
{
    host_test_env_teardown();
}

}

static void print_block(uint64_t lpa)
{
    char buffer[4096];
    do_pread(fd, buffer, 4096, lpa * 4096);
    print_journal_block(buffer);
}

extern "C" {

int comm_submit_sync_rw_request(comm_dev *dev, void *buffer, uint64_t lba, uint32_t lba_count, 
    comm_io_direction dir)
{
    std::lock_guard<std::mutex> lg(rw_mtx);
    size_t count = lba_count * LBA_SIZE;
    off_t offset = lba * LBA_SIZE;
    if (dir == comm_io_direction::COMM_IO_READ)
        do_pread(fd, buffer, count, offset);
    else
        do_pwrite(fd, buffer, count, offset);
    return 0;
}

int comm_submit_async_rw_request(comm_dev *dev, void *buffer, uint64_t lba, uint32_t lba_count,
    comm_async_cb_func cb_func, void *cb_arg, comm_io_direction dir)
{
    comm_submit_sync_rw_request(dev, buffer, lba, lba_count, dir);
    cb_func(comm_cmd_result::COMM_CMD_SUCCESS, cb_arg);
    return 0;
}

/* 模拟，直接返回fsImage上的数据 */
int comm_submit_sync_path_lookup_request(comm_dev *dev, path_lookup_task *task, size_t task_length, path_lookup_result *res)
{
    static std::unordered_map<std::string, uint32_t> dentry_ino_map = {
        {"a", 3}, {"b", 4}, {"c", 5}
    };

    static std::unordered_map<uint32_t, std::string> ino_next = {
        {2, "a"}, {3, "b"}, {4, "c"}
    };

    std::string path(task->path, task->pathlen);
    path_parser p_parser(path);
    uint32_t cur_idx = 0;
    uint32_t parent_ino = task->start_ino;

    for (auto it = p_parser.begin(); it != p_parser.end(); it.next(), ++cur_idx)
    {
        auto name = it.get();

        if (ino_next.count(parent_ino) == 0 || ino_next[parent_ino] != name)
        {
            res->path_inos[cur_idx] = INVALID_NID;
            if (it.is_last_component(p_parser.end()))
            {
                res->dentry_blkidx = 0;
                res->dentry_bitpos = 1;
            }
            break;
        }

        uint32_t cur_ino = dentry_ino_map.at(name);
        res->path_inos[cur_idx] = cur_ino;
        parent_ino = cur_ino;
        if (it.is_last_component(p_parser.end()))
        {
            res->dentry_blkidx = 0;
            res->dentry_bitpos = 0;
        }
    }

    return 0;
}

int comm_submit_sync_filemapping_search_request(comm_dev *dev, filemapping_search_task *task, 
    void *res, uint32_t res_len)
{
    static std::unordered_map<uint32_t, uint32_t> ino_lpa_map = {
        {2, 10}, {3, 11}, {4, 12}, {5, 13}
    };

    assert(task->nid_to_start == task->ino);
    assert(res_len == 4096);

    off_t offset = ino_lpa_map[task->ino] * 4096;
    do_pread(fd, res, res_len, offset);
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

/* mock获取头指针，将头指针推进upper(unapplied_lpa_num / 2)，并print出推进的日志内容 */
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
        // fmt::println(std::cout, "journal in LPA {}:", cur_head);
        // print_block(cur_head);
        // std::cout << std::endl;
        ++cur_head;
    }

    *head_lpa = cur_head;
    return 0;
}

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

}  // extern "C"

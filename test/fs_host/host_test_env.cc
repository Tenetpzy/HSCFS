#include "fs/fs_manager.hh"
#include "fs/fs.h"
#include "fs/path_utils.hh"
#include "cache/super_cache.hh"
#include "journal/journal_process_env.hh"
#include "communication/comm_api.h"
#include <mutex>
#include <string>
#include <stdexcept>
#include <unordered_map>

#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

using namespace hscfs;

int fd;
std::mutex rw_mtx;

#define LBA_SIZE 512

static void host_mock_init()
{
    fd = open("./fsImage", O_RDWR);
    if (fd == -1)
        throw std::runtime_error("open error");
}

void host_test_env_setup()
{
    host_mock_init();

    file_system_manager::init(nullptr);
    file_system_manager *fs_manager = file_system_manager::get_instance();
    super_cache &super = *(fs_manager->get_super_cache());
    
    journal_process_env::get_instance()->init(nullptr, super->meta_journal_start_blkoff, 
        super->meta_journal_end_blkoff, super->meta_journal_start_blkoff);
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
int comm_submit_sync_path_lookup_request(comm_dev *dev, path_lookup_task *task, path_lookup_result *res)
{
    static std::unordered_map<std::string, uint32_t> dentry_ino_map = {
        {"a", 3}, {"b", 4}, {"c", 5}
    };

    std::string path(task->path, task->pathlen);
    path_parser p_parser(path);
    uint32_t cur_idx = 0;

    
}

}  // extern "C"

#include "ssd/path_lookup.hh"
#include "communication/comm_api.h"
#include "communication/memory.h"
#include "utils/hscfs_exceptions.hh"
#include "utils/hscfs_log.h"
#include "path_lookup.hh"

namespace hscfs {

#ifdef CONFIG_PRINT_DEBUG_INFO
void print_path_lookup_task(const path_parser &path_parser, uint32_t start_ino, 
    path_dentry_iterator start_itr, uint32_t depth);
#endif

void ssd_path_lookup_controller::construct_task(const path_parser &path_parser, uint32_t start_ino, 
    path_dentry_iterator start_itr)
{
    uint32_t depth = 0;
    uint32_t total_dentry_len = 0;
    
    /* 统计一共有多少个带查找的目录项，以及目录项的长度和，便于分配path_lookup_task空间 */
    const path_dentry_iterator end_itr = path_parser.end();
    for (path_dentry_iterator cnt_itr = start_itr; cnt_itr != end_itr; cnt_itr.next())
    {
        ++depth;
        total_dentry_len += cnt_itr.get().length();
    }
    assert(depth != 0);
    assert(total_dentry_len != 0);

    #ifdef CONFIG_PRINT_DEBUG_INFO
    print_path_lookup_task(path_parser, start_ino, start_itr, depth);
    #endif

    /* 分配task空间，大小：path_lookup_task头部长度 + 路径字符串长度(所有目录项长度 + (depth-1)个'/'字符) */
    size_t path_len = total_dentry_len + depth - 1;
    size_t task_size = sizeof(path_lookup_task) + path_len; 
    char *buf = static_cast<char*>(comm_alloc_dma_mem(task_size));
    if (buf == nullptr)
        throw alloc_error("SSD path lookup controller: alloc task memory failed.");
    p_task_buf.reset(buf);

    /* 构造task头部 */
    path_lookup_task *p_header = reinterpret_cast<path_lookup_task*>(p_task_buf.get());
    p_header->start_ino = start_ino;
    p_header->depth = depth;
    p_header->pathlen = path_len;

    /* 构造路径字符串 */
    char *p_cur_entry = p_task_buf.get() + sizeof(path_lookup_task);
    for (; start_itr != end_itr; start_itr.next())
    {
        assert(p_cur_entry - buf < task_size);
        std::string dentry = start_itr.get();
        dentry.copy(p_cur_entry, dentry.length());
        p_cur_entry += dentry.length();
    }
}

void ssd_path_lookup_controller::do_pathlookup()
{
    assert(p_task_buf != nullptr);
    if (p_task_res_buf == nullptr)
    {
        char *buf = static_cast<char*>(comm_alloc_dma_mem(sizeof(path_lookup_result)));
        if (buf == nullptr)
            throw alloc_error("SSD path lookup controller: alloc task result memory failed.");
        p_task_res_buf.reset(buf);
    }
    int ret = comm_submit_sync_path_lookup_request(dev, reinterpret_cast<path_lookup_task*>(p_task_buf.get()), 
        reinterpret_cast<path_lookup_result*>(p_task_res_buf.get()));
    if (ret != 0)
        throw io_error("ssd path lookup controller: send path lookup task failed.");
}

uint32_t *ssd_path_lookup_controller::get_first_addr_of_result_inos() const noexcept
{
    return &(reinterpret_cast<path_lookup_result*>(p_task_res_buf.get())->path_inos[0]);
}

void path_lookup_buf_deletor::operator()(char *buf)
{
    comm_free_dma_mem(buf);
}

} // namespace hscfs
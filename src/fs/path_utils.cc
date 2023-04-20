#include "fs/path_utils.hh"
#include "fs/fs_manager.hh"
#include "fs/fs.h"
#include "communication/comm_api.h"
#include "communication/memory.h"
#include "utils/hscfs_exceptions.hh"
#include "utils/hscfs_log.h"

#include <cassert>
#include <memory>

/* SSD path lookup控制器 */
/*************************************************************************/
struct comm_dev;

namespace hscfs {

class path_lookup_buf_deletor
{
public:
    void operator()(char *buf);
};

class ssd_path_lookup_controller
{
public:
    ssd_path_lookup_controller(comm_dev *device)
    {
        dev = device;
    }

    /* 构造SSD路径解析命令 */
    void construct_task(const path_parser &path_parser, uint32_t start_ino, path_dentry_iterator start_itr);

    /* 进行路径解析，将结果保存到内部buf中 */
    void do_pathlookup();

    /* 返回结果中ino表的首元素地址 */
    uint32_t* get_first_addr_of_result_inos() const noexcept;
    
    /* to do */

private:
    comm_dev *dev;
    std::unique_ptr<char, path_lookup_buf_deletor> p_task_buf;
    std::unique_ptr<char, path_lookup_buf_deletor> p_task_res_buf;
};

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

/* SSD path lookup控制器 */
/******************************************************************************/


void path_dentry_iterator::next()
{
    if (nxt_start_pos == std::string::npos)
        get();
    cur_pos = nxt_start_pos;
    nxt_start_pos = std::string::npos;
}

std::string path_dentry_iterator::get()
{
    size_t start_pos = path_.find_first_not_of('/', cur_pos);
    if (start_pos == std::string::npos)
        start_pos = path_.length();
    size_t end_pos = path_.find_first_of('/', start_pos);
    if (end_pos == std::string::npos)
        end_pos = path_.length();

    // 缓存下一项的起始位置，调用nxt时就不必重复计算
    if (nxt_start_pos == std::string::npos)
        nxt_start_pos = end_pos;
    return path_.substr(start_pos, end_pos - start_pos);
}

bool path_dentry_iterator::is_pos_equal(size_t pos1, size_t pos2) const
{
    if (pos1 == pos2)
        return true;
    return path_.find_first_not_of('/', pos1) == path_.find_first_not_of('/', pos2);
}

std::string path_helper::extract_abs_path(const char *user_path)
{
    std::string path(user_path);

    #ifdef CONFIG_PATH_PREFIX
    
    static const std::string prefix(CONFIG_PATH_PREFIX);
    if (!is_prefix_valid(path, prefix))  // 检查是否有合法前缀
        throw user_path_invalid("invalid abs path.");
    if (path.length() <= prefix.length() || path[prefix.length()] != '/')  // 检查前缀后是否跟有'/'
        throw user_path_invalid("invalid abs path.");
    return path.substr(prefix.length());    

    #else

    if (path.length() == 0 || path[0] != '/')
        throw user_path_invalid("invalid abs path.");
    return path;

    #endif
}

std::string path_helper::extract_dir_path(const std::string &path)
{
    size_t pos = path.find_last_of('/');
    assert(pos != std::string::npos);
    return path.substr(0, pos + 1);
}

std::string path_helper::extract_file_name(const std::string &path)
{
    size_t pos = path.find_last_of('/');
    assert(pos != std::string::npos);
    return path.substr(pos + 1);
}

bool path_helper::is_prefix_valid(const std::string &user_path, const std::string &prefix)
{
    return user_path.find(prefix) == 0;
}

void path_lookup_processor::set_abs_path(const std::string &abs_path)
{
    start_dentry = fs_manager->get_root_dentry();
    path = abs_path;
}

dentry_handle path_lookup_processor::do_path_lookup()
{
    path_parser p_parser(path);
    dentry_cache *d_cache = fs_manager->get_dentry_cache();
    dentry_handle cur_dentry = start_dentry;  // cur_dentry为当前搜索到的目录项

    HSCFS_LOG(HSCFS_LOG_INFO, 
        "path lookup processor: lookup args:\n"
        "start inode: %u, path: %s",
        start_dentry->get_ino(), path.c_str()
    );

    for (auto itr = p_parser.begin(), end_itr = p_parser.end(); itr != end_itr; itr.next())
    {
        /* 如果当前目录项不是目录，则不再查找，返回不存在 */
        if (cur_dentry->get_type() != HSCFS_FT_DIR)
            return dentry_handle();

        std::string component_name = itr.get();  // component_name为下一项的名称
        dentry_handle component_dentry = d_cache->get(cur_dentry->get_ino(), component_name);

        /* 
         * 如果下一个目录项在缓存中不命中，交给SSD查找
         * 
         * 即使当前目录文件是脏的，也能交给SSD
         * 因为没写回SSD的目录项，由于淘汰保护，不会发生不命中
         * 发生不命中的都是目录中原有的目录项，而这部分在SSD侧仍然能正确访问
         */
        if (component_dentry.is_empty())
        {
            ssd_path_lookup_controller ctrlr(fs_manager->get_device());
            ctrlr.construct_task(p_parser, cur_dentry->get_ino(), itr);
            ctrlr.do_pathlookup();

            /* 将ssd查找的结果插入dentry cache，并直接在结果上继续path lookup */
            uint32_t *p_res_ino = ctrlr.get_first_addr_of_result_inos();
            for (; itr != end_itr; itr.next(), ++p_res_ino)
            {
                component_name = itr.get();

                /* 路径还没有搜索完，遇到了invalid_nid，则代表目录不存在，返回空handle */
                if (*p_res_ino == INVALID_NID)
                {
                    HSCFS_LOG(HSCFS_LOG_INFO, "path lookup processor: dentry [%u:%s] does not exist.", 
                        cur_dentry->get_ino(), component_name.c_str());
                    return dentry_handle();
                }

                HSCFS_LOG(HSCFS_LOG_INFO, "path lookup processor: result of SSD: dentry [%u:%s]'s inode is %u.",
                    cur_dentry->get_ino(), component_name.c_str(), *p_res_ino);
                
                /* 将component加入dentry cache，并置当前缓存项为component */
                cur_dentry = d_cache->add(cur_dentry->get_ino(), cur_dentry, *p_res_ino, component_name);
            }

            /* 在SSD返回的结果上成功完成了path lookup */
            return cur_dentry;
        }

        /* 下一个缓存项在缓存中找到了，置当前缓存项为下一个缓存项，继续 */
        HSCFS_LOG(HSCFS_LOG_INFO, "path lookup processor: dentry [%u:%s]'s inode is %u.",
            cur_dentry->get_ino(), component_name.c_str(), component_dentry->get_ino());
        cur_dentry = component_dentry;
    }

    /* 成功查找到了最后一级目录项，返回 */
    return cur_dentry;
}

} // namespace hscfs
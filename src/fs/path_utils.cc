#include "fs/path_utils.hh"
#include "fs/fs_manager.hh"
#include "fs/fs.h"
#include "fs/replace_protect.hh"
#include "fs/write_back_helper.hh"
#include "communication/comm_api.h"
#include "communication/memory.h"
#include "utils/dma_buffer_deletor.hh"
#include "utils/hscfs_exceptions.hh"
#include "utils/hscfs_log.h"

#include <cassert>
#include <memory>

/* SSD path lookup控制器 */
/*************************************************************************/
struct comm_dev;

namespace hscfs {

class ssd_path_lookup_controller
{
public:
    ssd_path_lookup_controller(file_system_manager *fs_manager)
    {
        this->fs_manager = fs_manager;
        dev = fs_manager->get_device();
    }

    /* 构造SSD路径解析命令，调用者必须保证start_ino对应文件存在 */
    void construct_task(const path_parser &path_parser, uint32_t start_ino, path_dentry_iterator start_itr);

    /* 进行路径解析，将结果保存到内部buf中。同步，等待命令执行完成后返回 */
    void do_pathlookup();

    /* 返回结果中ino表的首元素地址 */
    uint32_t* get_first_addr_of_result_inos() const noexcept;
    
    uint32_t get_result_depth() const noexcept
    {
        return depth_;
    }

    /*
     * 返回结果中目录项位置信息
     * 如果返回的信息有效(is_valid字段为true)，则调用方可根据目标是否存在，判断该位置信息是存储位置还是插入位置
     */
    dentry_store_pos get_result_pos() const noexcept;

private:
    file_system_manager *fs_manager;
    comm_dev *dev;
    std::unique_ptr<path_lookup_task, dma_buf_deletor> p_task_buf;
    std::unique_ptr<path_lookup_result, dma_buf_deletor> p_task_res_buf;
    uint32_t depth_;
    size_t task_length;
};

#ifdef CONFIG_PRINT_DEBUG_INFO
void print_path_lookup_task(path_lookup_task *task);
#endif

void ssd_path_lookup_controller::construct_task(const path_parser &path_parser, uint32_t start_ino, 
    path_dentry_iterator start_itr)
{
    auto upper_align = [](size_t siz, size_t align) {
        return (siz + align - 1) & (~(align - 1));
    };

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
    this->depth_ = depth;

    /* 分配task空间，大小：path_lookup_task头部长度 + 路径字符串长度(所有目录项长度 + (depth-1)个'/'字符) */
    size_t path_len = total_dentry_len + depth - 1;
    size_t task_size = sizeof(path_lookup_task) + path_len; 
    task_size = upper_align(task_size, 4);
    task_length = task_size;
    void *buf = comm_alloc_dma_mem(task_size);
    if (buf == nullptr)
        throw alloc_error("SSD path lookup controller: alloc task memory failed.");
    p_task_buf.reset(static_cast<path_lookup_task*>(buf));

    /* 构造task头部 */
    path_lookup_task *p_header = p_task_buf.get();
    p_header->start_ino = start_ino;
    p_header->depth = depth;
    p_header->pathlen = path_len;

    /* 构造路径字符串 */
    char *p_cur_entry = reinterpret_cast<char*>(p_task_buf.get()) + sizeof(path_lookup_task);
    for (; start_itr != end_itr; start_itr.next())
    {
        assert(static_cast<size_t>(p_cur_entry - static_cast<char*>(buf)) < task_size);
        std::string dentry = start_itr.get();
        dentry.copy(p_cur_entry, dentry.length());
        p_cur_entry += dentry.length();
        *p_cur_entry = '/';
        ++p_cur_entry;
    }

    #ifdef CONFIG_PRINT_DEBUG_INFO
    print_path_lookup_task(p_task_buf.get());
    #endif
}

void ssd_path_lookup_controller::do_pathlookup()
{
    assert(p_task_buf != nullptr);
    if (p_task_res_buf == nullptr)
    {
        void *buf = comm_alloc_dma_mem(sizeof(path_lookup_result));
        if (buf == nullptr)
            throw alloc_error("SSD path lookup controller: alloc task result memory failed.");
        p_task_res_buf.reset(static_cast<path_lookup_result*>(buf));
    }

    /* 需要等待SSD侧日志执行完成 */
    replace_protect_manager *rp_manager = fs_manager->get_replace_protect_manager();
    rp_manager->wait_all_journal_applied_in_SSD();

    int ret = comm_submit_sync_path_lookup_request(dev, p_task_buf.get(), task_length, p_task_res_buf.get());
    if (ret != 0)
        throw io_error("ssd path lookup controller: send path lookup task failed.");
}

uint32_t *ssd_path_lookup_controller::get_first_addr_of_result_inos() const noexcept
{
    return &(p_task_res_buf.get()->path_inos[0]);
}

dentry_store_pos ssd_path_lookup_controller::get_result_pos() const noexcept
{
    dentry_store_pos pos;
    path_lookup_result *result = p_task_res_buf.get();
    assert(result != nullptr);
    uint32_t *p_inos = get_first_addr_of_result_inos();
    size_t last_component = depth_ - 1;

    /* 如果目标文件存在，则结果中dentry位置有效 */
    if (p_inos[last_component] != INVALID_NID)
    {
        pos.blkno = result->dentry_blkidx;
        pos.slotno = result->dentry_bitpos;
        assert(pos.slotno != INVALID_DENTRY_BITPOS);
        pos.is_valid = true;
    }

    /* 如果目标文件不存在，但目标的目录存在 */
    else if (last_component == 0 || p_inos[last_component - 1] != INVALID_NID)
    {
        /* 如果目录中能找到目标的插入位置，则返回插入位置 */
        if (result->dentry_bitpos != INVALID_DENTRY_BITPOS)
        {
            pos.blkno = result->dentry_blkidx;
            pos.slotno = result->dentry_bitpos;
            pos.is_valid = true;
        }
    }

    return pos;
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

bool path_dentry_iterator::is_last_component(const path_dentry_iterator &end)
{
    path_dentry_iterator tmp = *this;
    tmp.next();
    return tmp == end;
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

dentry_handle path_lookup_processor::do_path_lookup(dentry_store_pos *pos_info)
{
    if (pos_info)
        pos_info->is_valid = false;
    path_parser p_parser(path);
    dentry_cache *d_cache = fs_manager->get_dentry_cache();
    dentry_handle cur_dentry = start_dentry;  // cur_dentry为当前搜索到的目录项

    HSCFS_LOG(HSCFS_LOG_INFO, 
        "path lookup processor: lookup args:\n"
        "start inode: %u, path: %s",
        start_dentry->get_ino(), path.c_str()
    );

    /* itr指向下一个目录项 */
    for (auto itr = p_parser.begin(), end_itr = p_parser.end(); itr != end_itr; itr.next())
    {
        /* 如果当前目录项不是目录，则不再查找，返回不存在 */
        if (cur_dentry->get_type() != HSCFS_FT_DIR)
        {
            HSCFS_LOG(HSCFS_LOG_INFO,
                "path lookup processor: half-way dentry [%u:%s] is not directory, path lookup terminated.",
                cur_dentry->get_key().dir_ino, cur_dentry->get_key().name.c_str()
            );
            return dentry_handle();
        }

        /* 如果当前目录项已经被删除，则不再查找 */
        if (cur_dentry->get_state() != dentry_state::valid)
        {
            HSCFS_LOG(HSCFS_LOG_INFO,
                "path lookup processor: half-way dentry [%u:%s] is deleted, path lookup terminated.",
                cur_dentry->get_key().dir_ino, cur_dentry->get_key().name.c_str()
            );
            return dentry_handle();
        }

        /* component_name为下一项的名称 */
        std::string component_name = itr.get();
        if (component_name == ".")
            continue;
        if (component_name == "..")
        {
            auto& parent_key = cur_dentry->get_parent_key();
            cur_dentry = d_cache->get(parent_key.dir_ino, parent_key.name);
            assert(cur_dentry.is_empty() == false);
            continue;
        }

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
            /* 
             * 如果cur_dentry是新建的，则要把全部元数据写回SSD后再下发path lookup命令
             * 否则，cur_dentry在SSD上还不存在，SSD将访问错误的NAT表和文件数据
             */
            if (cur_dentry->is_newly_created())
            {
                write_back_helper wb_helper(fs_manager);
                wb_helper.write_meta_back_sync();
                replace_protect_manager *rp_manager = fs_manager->get_replace_protect_manager();
                rp_manager->wait_all_journal_applied_in_SSD();
                assert(cur_dentry->is_newly_created() == false);
            }

            HSCFS_LOG(HSCFS_LOG_INFO, "path lookup processor: dentry [%u:%s] miss, prepare searching in SSD.", 
                cur_dentry->get_ino(), component_name.c_str());
            ssd_path_lookup_controller ctrlr(fs_manager);
            ctrlr.construct_task(p_parser, cur_dentry->get_ino(), itr);
            ctrlr.do_pathlookup();

            /* ssd_depth和cur_depth用作正确性检查 */
            uint32_t ssd_depth = ctrlr.get_result_depth();
            uint32_t cur_depth = 0;
            uint32_t *p_res_ino = ctrlr.get_first_addr_of_result_inos();

            /* 将ssd查找的结果插入dentry cache，并直接在结果上继续path lookup */
            for (; itr != end_itr; itr.next(), ++p_res_ino, ++cur_depth)
            {
                component_name = itr.get();

                if (component_name == ".")
                {
                    assert(*p_res_ino == cur_dentry->get_ino());
                    continue;
                }
                if (component_name == "..")
                {
                    assert(*p_res_ino == cur_dentry->get_key().dir_ino);
                    auto &parent_key = cur_dentry->get_parent_key();
                    cur_dentry = d_cache->get(parent_key.dir_ino, parent_key.name);
                    assert(cur_dentry.is_empty() == false);
                    continue;
                }

                /* 路径还没有搜索完，遇到了invalid_nid，则代表目标不存在，返回空handle */
                if (*p_res_ino == INVALID_NID)
                {
                    HSCFS_LOG(HSCFS_LOG_INFO, "path lookup processor: dentry [%u:%s] does not exist.", 
                        cur_dentry->get_ino(), component_name.c_str());
                    
                    /* 如果已经找到了目标的目录，则SSD有可能返回创建目标的位置信息，把它保存到pos_info中 */
                    if (itr.is_last_component(end_itr))
                    {
                        dentry_store_pos ssd_pos_info = ctrlr.get_result_pos();
                        if (ssd_pos_info.is_valid)
                        {
                            HSCFS_LOG(HSCFS_LOG_INFO, "path_lookup_processor: target dentry [%s] does not exist, "
                                "but its parent dentry [%s] exist, the location for creating target returned by SSD:\n"
                                "block offset: %u, slot offset: %u.",
                                component_name.c_str(), cur_dentry->get_key().name.c_str(), 
                                ssd_pos_info.blkno, ssd_pos_info.slotno
                            );
                        }

                        if (pos_info)
                            *pos_info = ssd_pos_info;
                    }

                    return dentry_handle();
                }

                HSCFS_LOG(HSCFS_LOG_INFO, "path lookup processor: result of SSD: dentry [%u:%s]'s inode is %u.",
                    cur_dentry->get_ino(), component_name.c_str(), *p_res_ino);
                
                /* 将component加入dentry cache，并置当前缓存项为component */
                cur_dentry = d_cache->add(cur_dentry->get_ino(), cur_dentry, *p_res_ino, component_name);
            }

            assert(cur_depth == ssd_depth);

            /* 在SSD返回的结果上成功完成了path lookup，把位置信息附加到最终的目录项上 */
            dentry_store_pos ssd_pos_info = ctrlr.get_result_pos();
            HSCFS_LOG(HSCFS_LOG_INFO, "path lookup processor: target dentry [%s]'s pos info returned by SSD:\n"
                "block offset: %u, slot offset: %u.",
                cur_dentry->get_key().name.c_str(), ssd_pos_info.blkno, ssd_pos_info.slotno
            );
            if (pos_info)
                *pos_info = ssd_pos_info;
            cur_dentry->set_pos_info(ssd_pos_info);
            
            return cur_dentry;
        }

        /* 下一个目录项在缓存中找到了，置当前目录项为下一个目录项，继续 */
        HSCFS_LOG(HSCFS_LOG_INFO, "path lookup processor: dentry [%u:%s] is in dentry cache, its inode is %u.",
            cur_dentry->get_ino(), component_name.c_str(), component_dentry->get_ino());
        cur_dentry = component_dentry;
    }

    /* 成功查找到了最后一级目录项，返回 */
    return cur_dentry;
}

} // namespace hscfs
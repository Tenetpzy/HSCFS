#include <cassert>
#include "fs/path_utils.hh"
#include "fs/fs_manager.hh"
#include "fs/fs.h"
#include "utils/hscfs_exceptions.hh"
#include "utils/hscfs_log.h"
#include "ssd/path_lookup.hh"
#include "path_utils.hh"

namespace hscfs {

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
        throw user_path_invalid();
    if (path.length() <= prefix.length() || path[prefix.length()] != '/')  // 检查前缀后是否跟有'/'
        throw user_path_invalid();
    return path.substr(prefix.length());    

    #else

    if (path.length() == 0 || path[0] != '/')
        throw user_path_invalid();
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

    for (auto itr = p_parser.begin(), end_itr = p_parser.end(); itr != end_itr; itr.next())
    {
        /* 如果当前目录项不是目录，则不再查找，返回不存在 */
        if (cur_dentry->get_type() != HSCFS_FT_DIR)
            return dentry_handle();

        std::string component_name = itr.get();  // component_name为下一项的名称
        dentry_handle component_dentry = d_cache->get(cur_dentry->get_ino(), component_name);

        /* 如果下一个目录项在缓存中不命中 */
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
                cur_dentry = d_cache->add(cur_dentry->get_ino(), cur_dentry, *p_res_ino, component_name, fs_manager);
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
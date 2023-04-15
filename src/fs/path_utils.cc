#include <cassert>
#include "fs/path_utils.hh"
#include "fs/fs_manager.hh"
#include "utils/hscfs_exceptions.hh"

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

dentry_handle path_lookup_processor::do_path_lookup()
{
    path_parser p_parser(abs_path);
    dentry_cache *d_cache = fs_manager->get_dentry_cache();
    dentry_handle cur_dir = fs_manager->get_root_dentry();

    for (auto itr = p_parser.begin(), end_itr = p_parser.end(); itr != end_itr; )
    {
        std::string component_name = itr.get();
        dentry_handle component_dentry = d_cache->get(cur_dir->get_ino(), component_name);
        
        // 如果当前目录项在缓存中不命中，则交给SSD进行查找
        if (component_dentry.is_empty())
        {
            
        }
    }
}

} // namespace hscfs
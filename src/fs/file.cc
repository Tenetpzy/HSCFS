#include <system_error>

#include "cache/page_cache.hh"
#include "cache/node_block_cache.hh"
#include "fs/file.hh"
#include "fs/fs_manager.hh"
#include "fs/file_utils.hh"
#include "utils/hscfs_exceptions.hh"
#include "utils/exception_handler.hh"
#include "utils/hscfs_log.h"
#include "utils/lock_guards.hh"
#include "file.hh"

namespace hscfs {

file::file(uint32_t ino, const dentry_handle &dentry, file_system_manager *fs_manager)
{
    this->ino = ino;
    this->fs_manager = fs_manager;
    int ret = spin_init(&file_meta_lock);
    if (ret != 0)
        throw std::system_error(std::error_code(ret, std::generic_category()), 
            "file object: init file meta lock failed.");
    ret = rwlock_init(&file_op_lock);
    if (ret != 0)
    {
        spin_destroy(&file_meta_lock);
        throw std::system_error(std::error_code(ret, std::generic_category()), 
            "file object: init file op lock failed.");
    }
    page_cache_.reset(new page_cache(fs_manager->get_page_cache_size()));
    ref_count = 0;
    fd_ref_count = 0;
    is_dirty = false;
    this->dentry = dentry;
}

file::~file()
{
    if (is_dirty)
        HSCFS_LOG(HSCFS_LOG_WARNING, "file object is still dirty while destructed.");
    if (ref_count)
        HSCFS_LOG(HSCFS_LOG_WARNING, "file object has non-zero refcount which equals %u while destructed.", 
            ref_count.load());
    assert(fd_ref_count <= ref_count);
    spin_destroy(&file_meta_lock);
    rwlock_destroy(&file_op_lock);
}

void file::add_fd_refcount()
{
    ++fd_ref_count;
    dentry->add_fd_refcount();
}

void file::sub_fd_refcount()
{
    --fd_ref_count;
    dentry->sub_fd_refcount();
}

bool file::truncate(size_t tar_size)
{
    /* 修改文件的索引以适应新大小 */
    auto inode_handle = node_cache_helper(fs_manager).get_node_entry(ino, INVALID_NID);
    hscfs_inode *inode = &inode_handle->get_node_block_ptr()->i;
    size_t i_size = inode->i_size;

    file_resizer resizer(fs_manager);
    if (i_size < tar_size)
        resizer.expand(ino, tar_size);
    else if (i_size > tar_size)
        resizer.reduce(ino, tar_size);
    else
        return false;

    /* 修改file内元数据，由于已经获取了file_op_lock独占，所以不用再加file_meta_lock锁了 */
    size = tar_size;
    mark_modified();
    return true;
}

ssize_t file::read(char *buffer, ssize_t count, uint64_t pos)
{
    /* 获得文件操作共享锁，获取后，文件长度保证不会减小(truncate需要获取此独占锁) */
    rwlock_guard file_op_lg(file_op_lock, rwlock_guard::lock_type::rdlock);

    const uint64_t cur_size = get_cur_size();  // 获取一致的文件大小
    if (pos >= cur_size)
        return 0;

    /* 依次遍历读取范围内的每一个块 */
    ssize_t read_count = 0;
    std::unique_lock<std::mutex> pre_page_lock;  // 前一个page的锁。获取到当前page的锁后再释放
    while (read_count < count && pos < cur_size)
    {
        /* 计算当前块偏移cur_blkno和当前块内的读取范围[pos, end_pos) */
        uint32_t cur_blkno = idx_of_blk(pos);
        uint64_t end_pos = std::min(end_pos_of_cur_blk(pos), cur_size);

        /* 从缓存中获取当前page，加锁，解除上一个page的锁，准备好当前page的内容(如从SSD读) */
        page_entry_handle cur_page = page_cache_->get(cur_blkno);
        std::unique_lock<std::mutex> cur_page_lock(cur_page->get_page_lock());
        pre_page_lock = std::move(cur_page_lock);  // 解锁pre_page_lock，并接管cur_page_lock
        prepare_page_content(cur_page);

        
    }
}

bool file::mark_dirty()
{
    bool expect = false;
    return is_dirty.compare_exchange_strong(expect, true);
}

#ifdef CONFIG_PRINT_DEBUG_INFO
void print_inode_meta(uint32_t ino, hscfs_inode *inode);
#endif

void file::read_meta()
{
    node_cache_helper node_helper(fs_manager);
    node_block_cache_entry_handle inode_handle = node_helper.get_node_entry(ino, INVALID_NID);
    hscfs_node *node = inode_handle->get_node_block_ptr();
    hscfs_inode *inode = &node->i;

    assert(ino == node->footer.ino);
    assert(ino == node->footer.nid);
    assert(0 == node->footer.offset);

    #ifdef CONFIG_PRINT_DEBUG_INFO
    print_inode_meta(ino, inode);
    #endif

    {
        spin_lock_guard lg(file_meta_lock);
        size = inode->i_size;
        nlink = inode->i_nlink;
        atime.tv_sec = inode->i_atime;
        atime.tv_nsec = inode->i_atime_nsec;
        mtime.tv_sec = inode->i_mtime;
        mtime.tv_nsec = inode->i_mtime_nsec;

        is_dirty = false;
    }
}

void file::mark_access()
{
    timespec_get(&atime, TIME_UTC);
}

void file::mark_modified()
{
    timespec t;
    timespec_get(&t, TIME_UTC);
    atime = t;
    mtime = t;
}

uint64_t file::get_cur_size()
{
    spin_lock_guard lg(file_meta_lock);
    return size;
}

void file::prepare_page_content(page_entry_handle &page)
{
    /* to do... */

    /* page内容有效，直接返回 */
    if (page->get_state() == page_state::ready)
        return;
    
    /* page内容无效，判断是应该从SSD读，还是应该直接初始化 */
    page->set_state(page_state::ready);
    std::lock_guard<std::mutex> fs_meta_lg(fs_manager->get_fs_meta_lock());
    uint32_t blkoff = page->get_blkoff();

    /* 获取该文件的inode block，得到当前inode中文件的最大块偏移 */
    node_cache_helper node_helper(fs_manager);
    auto inode_handle = node_helper.get_node_entry(ino, INVALID_NID);
    hscfs_inode *inode = &inode_handle->get_node_block_ptr()->i;
    uint64_t size_in_inode = inode->i_size;
    uint64_t max_blkoff_in_inode = SIZE_TO_BLOCK(size_in_inode) - 1;

    /* page超出了文件块偏移范围，origin_lpa和commit_lpa都为INVALID_LPA，内容初始化为0即可(block_buffer构造时自动完成) */
    if (blkoff > max_blkoff_in_inode)
    {
        page->set_origin_lpa(INVALID_LPA);
        page->set_commit_lpa(INVALID_LPA);
        return;
    }

    /* page在文件块偏移范围内，通过文件索引查询找到它的LPA */
    block_addr_info page_addr = file_mapping_util(fs_manager).get_addr_of_block(ino, blkoff);

    /* 如果page在文件空洞范围内，origin_lpa和commit_lpa都为INVALID_LPA，内容初始化为0 */
    if (page_addr.lpa == INVALID_LPA)
    {
        HSCFS_LOG(HSCFS_LOG_DEBUG, "page offset %u of file(ino = %u) is in file holes.", blkoff, ino);
        page->set_origin_lpa(INVALID_LPA);
        page->set_commit_lpa(INVALID_LPA);
        return;        
    }

    /* 否则，初始化其origin_lpa，从SSD读出内容 */
    HSCFS_LOG(HSCFS_LOG_DEBUG, "the LPA of page offset %u in file(ino = %u) is %u.", blkoff, ino, page_addr.lpa);
    page->set_origin_lpa(page_addr.lpa);
    page->set_commit_lpa(INVALID_LPA);
    page->get_page_buffer().read_from_lpa(fs_manager->get_device(), page_addr.lpa);
}

file_handle file_obj_cache::add(uint32_t ino, const dentry_handle &dentry)
{
    auto p_entry = std::make_unique<file>(ino, dentry, fs_manager);
    assert(cache_manager.get(ino) == nullptr);
    file *raw_p = p_entry.get();
    cache_manager.add(ino, p_entry);
    ++cur_size;
    add_refcount(raw_p);
    do_relpace();
    return file_handle(raw_p, this);
}

file_handle file_obj_cache::get(uint32_t ino)
{
    auto p_entry = cache_manager.get(ino);
    if (p_entry != nullptr)
        add_refcount(p_entry);
    return file_handle(p_entry, this);
}

void file_obj_cache::add_refcount(file *entry)
{
    if (++entry->ref_count == 1)
        cache_manager.pin(entry->ino);
}

void file_obj_cache::sub_refcount(file *entry)
{
    if (--entry->ref_count == 0)
        cache_manager.unpin(entry->ino);
}

void file_obj_cache::do_relpace()
{
    if (cur_size > expect_size)
    {
        while (true)
        {
            auto p = cache_manager.replace_one();
            if (p != nullptr)
            {
                assert(p->ref_count == 0);
                --cur_size;
                HSCFS_LOG(HSCFS_LOG_INFO, "replace file object, inode = %u",
                    p->ino);
            }
            if (p == nullptr || cur_size <= expect_size)
                break;
        }
    }
}

void file_obj_cache::add_to_dirty_files(const file_handle &file)
{
    spin_lock_guard lg(dirty_files_lock);
    uint32_t ino = file.entry->ino;
    assert(dirty_files.count(ino) == 0);
    dirty_files.emplace(ino, file);
}

file_handle::~file_handle()
{
    if (entry != nullptr)
    {
        try
        {
            cache->sub_refcount(entry);
        }
        catch(const std::exception &e)
        {
            HSCFS_LOG(HSCFS_LOG_WARNING, "exception during sub_refcount of file object: "
                "%s", e.what());
        }
    }
}

void file_handle::mark_dirty()
{
    if (entry->mark_dirty())
        cache->add_to_dirty_files(*this);
}

void file_handle::do_addref()
{
    if (entry != nullptr)
        cache->add_refcount(entry);
}

void file_handle::do_subref()
{
    if (entry != nullptr)
        cache->sub_refcount(entry);
}

file_handle file_cache_helper::get_file_obj(uint32_t ino, const dentry_handle &dentry)
{
    file_handle target_file = file_cache->get(ino);

    /*
     * 如果file缓存不命中，则先在缓存中创建一个file对象
     * 然后把它的元数据读到file对象中
     * 只要外部使用file_cache_helper获取file对象，则始终能保证该对象内元数据有效 
     */
    if (target_file.is_empty())
    {
        target_file = file_cache->add(ino, dentry);
        target_file->read_meta();
    }

    return target_file;
}

} // namespace hscfs
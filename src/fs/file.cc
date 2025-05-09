#include <system_error>
#include <cstring>

#include "cache/page_cache.hh"
#include "cache/node_block_cache.hh"
#include "fs/file.hh"
#include "fs/fs_manager.hh"
#include "fs/file_utils.hh"
#include "fs/write_back_helper.hh"
#include "fs/srmap_utils.hh"
#include "utils/hscfs_exceptions.hh"
#include "utils/exception_handler.hh"
#include "utils/hscfs_log.h"
#include "utils/lock_guards.hh"
#include "utils/io_utils.hh"

namespace hscfs {

file::file(uint32_t ino, file_system_manager *fs_manager)
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

    /* 将page cache内多余的page置为INVALID状态 */
    page_cache_->truncate(SIZE_TO_BLOCK(tar_size) - 1);

    return true;
}

ssize_t file::read(char *buffer, ssize_t count, uint64_t pos)
{
    const uint64_t cur_size = get_cur_size();  // 获取一致的page cache中文件大小
    const uint64_t read_end_pos = std::min(cur_size, pos + count);
    ssize_t read_count = 0;  // 当前已经读取的字节数

    {
        page_entry_handle pre_page;  // 必须保持上一个page的引用，确保解锁上一个page时pre_page_lock不是悬垂引用
        std::unique_lock<std::mutex> pre_page_lock;  // 前一个page的锁。获取到当前page的锁后再释放(必须定义在pre_page后)
        
        while (pos < read_end_pos)
        {
            /* 计算当前块偏移cur_blkno和当前块内需读取的文件偏移范围[pos, end_pos) */
            const uint32_t cur_blkno = idx_of_blk(pos);
            const uint64_t end_pos = std::min(end_pos_of_cur_blk(pos), read_end_pos);
            assert(end_pos > pos && end_pos - pos <= 4096);
            HSCFS_LOG(HSCFS_LOG_DEBUG, "read in file(inode = %u), blkno %u, range [%u, %u).", ino, cur_blkno, pos, end_pos);

            /* 从缓存中获取当前page，加锁，解除上一个page的锁，准备好当前page的内容(如从SSD读) */
            page_entry_handle cur_page = page_cache_->get(cur_blkno);
            std::unique_lock<std::mutex> cur_page_lock(cur_page->get_page_lock());
            pre_page_lock = std::move(cur_page_lock);  // 解锁pre_page_lock，并接管cur_page_lock
            prepare_page_content(cur_page);

            /* 从page中拷贝内容到用户缓冲区 */
            const size_t cp_start_off = off_in_blk(pos);
            const size_t cp_cnt = end_pos - pos;
            char *page_buffer = cur_page->get_page_buffer().get_ptr();
            std::memcpy(buffer + read_count, page_buffer + cp_start_off, cp_cnt);

            read_count += cp_cnt;
            pos += cp_cnt;
            pre_page = std::move(cur_page);
        }
        /* pre_page_lock析构，自动释放最后一个page的锁 */
    }

    mark_access();  // 更新atime

    return read_count;
}

ssize_t file::write(char *buffer, ssize_t count, uint64_t pos)
{
    const uint64_t write_end_pos = pos + count;  // 写入范围的尾后位置
    ssize_t write_count = 0;  // 当前已经写入的字节数

    /* 依次遍历写入范围内的每一个page(包括超过文件当前page cache大小的page) */
    {
        page_entry_handle pre_page;  // 必须保持上一个page的引用，确保解锁上一个page时pre_page_lock不是悬垂引用
        std::unique_lock<std::mutex> pre_page_lock;  // 前一个page的锁。获取到当前page的锁后再释放(必须定义在pre_page后)
        
        while (write_count < count)
        {
            /* 计算当前块偏移cur_blkno和当前块内需写入的文件偏移范围[pos, end_pos) */
            assert(pos < write_end_pos);
            const uint32_t cur_blkno = idx_of_blk(pos);
            const uint64_t end_pos = std::min(end_pos_of_cur_blk(pos), write_end_pos);
            assert(end_pos > pos && end_pos - pos <= 4096);
            HSCFS_LOG(HSCFS_LOG_DEBUG, "write in file(inode = %u), blkno %u, range [%u, %u).", ino, cur_blkno, pos, end_pos);

            /* 从缓存中获取当前page，加锁，解除上一个page的锁，准备好当前page的内容(如从SSD读) */
            page_entry_handle cur_page = page_cache_->get(cur_blkno);
            std::unique_lock<std::mutex> cur_page_lock(cur_page->get_page_lock());
            pre_page_lock = std::move(cur_page_lock);  // 解锁pre_page_lock，并接管cur_page_lock
            prepare_page_content(cur_page);

            /* 从用户缓冲区拷贝内容到page中 */
            const size_t cp_start_off = off_in_blk(pos);
            const size_t cp_cnt = end_pos - pos;
            char *page_buffer = cur_page->get_page_buffer().get_ptr();
            std::memcpy(page_buffer + cp_start_off, buffer + write_count, cp_cnt);
            cur_page.mark_dirty();

            write_count += cp_cnt;
            pos += cp_cnt;
            pre_page = std::move(cur_page);
        }
        /* pre_page_lock析构，自动释放最后一个page的锁 */
    }

    /* 更新访问时间和文件大小 */
    set_cur_size_if_larger(write_end_pos);
    mark_modified();
    return write_count;
}

void file::write_back()
{
    update_meta_to_inode();

    auto &dirty_pages = page_cache_->get_dirty_pages();

    write_back_helper wb_helper(fs_manager);
    file_mapping_util fm_util(fs_manager);
    srmap_utils *srmap_util = fs_manager->get_srmap_util();
    async_vecio_synchronizer syn(dirty_pages.size());

    for (auto &entry: dirty_pages)
    {
        page_entry_handle &page_handle = entry.second;
        
        /* 将page写回SSD */
        uint32_t new_lpa = wb_helper.do_write_back_async(page_handle->get_page_buffer(), page_handle->get_lpa_ref(), 
            write_back_helper::block_type::data, async_vecio_synchronizer::generic_callback, &syn);
        assert(new_lpa == page_handle->get_lpa_ref());

        /* 更新file mapping */
        block_addr_info addr = fm_util.update_block_mapping(ino, page_handle->get_blkoff(), new_lpa);

        /* 写入SRMAP表 */
        srmap_util->write_srmap_of_data(new_lpa, ino, page_handle->get_blkoff());
    }

    /* 等待所有异步I/O完成 */
    comm_cmd_result res = syn.wait_cplt();
    if (res != comm_cmd_result::COMM_CMD_SUCCESS)
        throw io_error("write back page cache failed.");
    
    page_cache_->clear_dirty_pages();  // 清除页面的脏标记
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
    assert(inode->i_type == HSCFS_FT_REG_FILE);

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
    timespec t;
    timespec_get(&t, TIME_UTC);
    spin_lock_guard lg(file_meta_lock);
    atime = t;
}

void file::mark_modified()
{
    timespec t;
    timespec_get(&t, TIME_UTC);
    spin_lock_guard lg(file_meta_lock);
    atime = t;
    mtime = t;
}

uint64_t file::get_cur_size()
{
    spin_lock_guard lg(file_meta_lock);
    return size;
}

void file::set_cur_size_if_larger(uint64_t size_after_write)
{
    spin_lock_guard lg(file_meta_lock);
    if (size_after_write > size)
        size = size_after_write;
}

void file::prepare_page_content(page_entry_handle &page)
{
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

    /* page超出了文件块偏移范围，lpa为INVALID_LPA，内容初始化为0即可(block_buffer构造时自动完成) */
    if (blkoff > max_blkoff_in_inode)
    {
        HSCFS_LOG(HSCFS_LOG_DEBUG, "page offset %u of file(ino = %u) is beyond file size(%u bytes).", 
            blkoff, ino, size_in_inode);
        page->set_lpa(INVALID_LPA);
        return;
    }

    /* page在文件块偏移范围内，通过文件索引查询找到它的LPA */
    block_addr_info page_addr = file_mapping_util(fs_manager).get_addr_of_block(ino, blkoff);

    /* 如果page在文件空洞范围内，lpa为INVALID_LPA，内容初始化为0 */
    if (page_addr.lpa == INVALID_LPA)
    {
        HSCFS_LOG(HSCFS_LOG_DEBUG, "page offset %u of file(ino = %u) is in file holes.", blkoff, ino);
        page->set_lpa(INVALID_LPA);
        return;        
    }

    /* 否则，初始化其lpa，从SSD读出内容 */
    HSCFS_LOG(HSCFS_LOG_DEBUG, "the LPA of page offset %u in file(ino = %u) is %u.", blkoff, ino, page_addr.lpa);
    page->set_lpa(page_addr.lpa);
    page->get_page_buffer().read_from_lpa(fs_manager->get_device(), page_addr.lpa);
}

void file::update_meta_to_inode()
{
    node_cache_helper node_helper(fs_manager);
    auto inode_handle = node_helper.get_node_entry(ino, INVALID_NID);
    hscfs_inode *inode = &inode_handle->get_node_block_ptr()->i;
    if (size > inode->i_size)
    {
        file_resizer resizer(fs_manager);
        resizer.expand(ino, size);
    }
    inode->i_atime = atime.tv_sec;
    inode->i_atime_nsec = atime.tv_nsec;
    inode->i_mtime = mtime.tv_sec;
    inode->i_mtime_nsec = mtime.tv_nsec;
    inode_handle.mark_dirty();
}

file_obj_cache::file_obj_cache(size_t expect_size, file_system_manager *fs_manager)
{
    this->expect_size = expect_size;
    this->fs_manager = fs_manager;
    cur_size = 0;
    int ret = spin_init(&dirty_files_lock);
    if (ret != 0)
        throw std::system_error(std::error_code(ret, std::generic_category()), 
            "file object cache: init dirty files lock failed.");
}

file_obj_cache::~file_obj_cache()
{
    if (!dirty_files.empty())
        HSCFS_LOG(HSCFS_LOG_WARNING, "file obj cache still has dirty files when destructed.");
}

file_handle file_obj_cache::add(uint32_t ino)
{
    auto p_entry = std::make_unique<file>(ino, fs_manager);
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

bool file_obj_cache::contains(uint32_t ino)
{
    return cache_manager.get(ino) != nullptr;
}

std::unordered_map<uint32_t, file_handle> file_obj_cache::get_and_clear_dirty_files()
{
    spin_lock_guard dirty_files_lg(dirty_files_lock);
    for (auto &entry : dirty_files)
    {
        file_handle &handle = entry.second;
        handle.entry->is_dirty = false;
    }

    std::unordered_map<uint32_t, file_handle> ret;
    dirty_files.swap(ret);
    assert(dirty_files.empty());
    return ret;
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

void file_obj_cache::remove_from_dirty_files(const file_handle &file)
{
    spin_lock_guard lg(dirty_files_lock);
    uint32_t ino = file.entry->ino;
    assert(dirty_files.count(ino) == 1);
    dirty_files.erase(ino);
}

void file_obj_cache::remove_file(file *entry)
{
    HSCFS_LOG(HSCFS_LOG_DEBUG, "remove file object(inode = %u) from file obj cache.", entry->ino);

    /* 如果file是dirty的，把它从dirty set中移除 */
    if (entry->is_dirty)
    {
        spin_lock_guard dirty_files_lg(dirty_files_lock);
        assert(dirty_files.count(entry->ino) == 1);
        dirty_files.erase(entry->ino);
        entry->is_dirty = false;
    }

    /* 在缓存中移除file对象 */
    assert(entry->ref_count == 1);  // 此方法必然由最后一个引用file的handle调用，因此ref_count只可能为1
    entry->ref_count = 0;
    cache_manager.unpin(entry->ino);
    uint32_t ino = entry->ino;
    cache_manager.remove(ino);  // 此时file对象析构, entry参数变为悬挂指针
    --cur_size;
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

void file_handle::clear_dirty()
{
    if (entry->is_dirty)
    {
        entry->is_dirty = 0;
        cache->remove_from_dirty_files(*this);
    }
}

void file_handle::write_back()
{
    clear_dirty();
    entry->write_back();
}

void file_handle::delete_file()
{
    assert(entry->nlink == 0);
    assert(entry->fd_ref_count == 0);
    assert(entry->ref_count <= 2);
    file_deletor(entry->fs_manager).delete_file(entry->ino);
    cache->remove_file(entry);
    entry = nullptr;
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

file_handle file_cache_helper::get_file_obj(uint32_t ino)
{
    file_handle target_file = file_cache->get(ino);

    /*
     * 如果file缓存不命中，则先在缓存中创建一个file对象
     * 然后把它的元数据读到file对象中
     * 只要外部使用file_cache_helper获取file对象，则始终能保证该对象内元数据有效 
     */
    if (target_file.is_empty())
    {
        target_file = file_cache->add(ino);
        target_file->read_meta();
    }

    return target_file;
}

} // namespace hscfs
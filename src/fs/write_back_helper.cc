#include "cache/block_buffer.hh"
#include "cache/node_block_cache.hh"
#include "cache/dir_data_block_cache.hh"
#include "fs/write_back_helper.hh"
#include "fs/fs_manager.hh"
#include "fs/SIT_utils.hh"
#include "fs/NAT_utils.hh"
#include "fs/srmap_utils.hh"
#include "fs/file_utils.hh"
#include "fs/super_manager.hh"
#include "fs/fs.h"
#include "fs/super_manager.hh"
#include "fs/replace_protect.hh"
#include "journal/journal_container.hh"
#include "journal/journal_process_env.hh"
#include "utils/io_utils.hh"
#include "utils/hscfs_log.h"

namespace hscfs {

write_back_helper::write_back_helper(file_system_manager *fs_manager)
    : sit_operator(fs_manager)
{
    this->fs_manager = fs_manager;
    this->super = fs_manager->get_super_manager();
}

uint32_t write_back_helper::do_write_back_async(block_buffer &buffer, uint32_t &lpa, block_type type,
                                                comm_async_cb_func cb_func, void *cb_arg)
{
    uint32_t new_lpa;
    if (type == block_type::data)
        new_lpa = super->alloc_data_lpa();
    else
        new_lpa = super->alloc_node_lpa();
    
    if (lpa != INVALID_LPA)
        sit_operator.invalidate_lpa(lpa);
    
    lpa = new_lpa;
    buffer.write_to_lpa_async(fs_manager->get_device(), lpa, cb_func, cb_arg);

    return lpa;
}

void write_back_helper::write_meta_back_sync()
{
    std::list<node_block_cache_entry_handle> dirty_nodes = fs_manager->get_node_cache()->get_and_clear_dirty_list();
    std::unordered_map<uint32_t, std::vector<dir_data_block_handle>> dirty_dir_blks = fs_manager->get_dir_data_cache()
        ->get_and_clear_dirty_blks();

    /* 统计node和dir data block的块I/O数 */
    uint64_t io_num = 0;
    io_num += dirty_nodes.size();
    for (auto &entry : dirty_dir_blks)
    {
        uint64_t siz = entry.second.size();
        assert(siz > 0);
        io_num += siz;
    }
    
    async_vecio_synchronizer syn(io_num);
    srmap_utils *srmap_util = fs_manager->get_srmap_util();
    nat_lpa_mapping nat_map(fs_manager);
    file_mapping_util fm_util(fs_manager);

    /* 回写dirty node */
    for (auto &node_handle : dirty_nodes)
    {
        uint32_t nid = node_handle->get_nid();
        HSCFS_LOG(HSCFS_LOG_INFO, "writing back node block(nid = %u).", nid);

        /* 将node buffer异步写入SSD */
        uint32_t new_lpa = do_write_back_async(node_handle->get_node_buffer(), node_handle->get_lpa_ref(), 
            block_type::node, async_vecio_synchronizer::generic_callback, &syn); 
        
        /* 更新NAT表 */
        nat_map.set_lpa_of_nid(nid, new_lpa);

        /* 更新SRMAP表 */
        srmap_util->write_srmap_of_node(new_lpa, nid);
    }

    /* 回写dirty dir data block */
    for (auto &entry : dirty_dir_blks)
    {
        uint32_t dir_ino = entry.first;
        std::vector<dir_data_block_handle> &dirty_list = entry.second;
        for (auto &blk_handle : dirty_list)
        {
            assert(blk_handle->get_key().ino == dir_ino);
            uint32_t blkoff = blk_handle->get_key().blkoff;
            HSCFS_LOG(HSCFS_LOG_INFO, "writing back dir data block(ino=%u, blkoff=%u).", dir_ino, blkoff);

            /* 将data block异步写入SSD */
            uint32_t new_lpa = do_write_back_async(blk_handle->get_block_buffer(), blk_handle->get_lpa_ref(), 
                block_type::data, async_vecio_synchronizer::generic_callback, &syn);
            
            /* 更新file mapping */
            block_addr_info addr = fm_util.update_block_mapping(dir_ino, blkoff, new_lpa);

            /* 更新srmap表 */
            srmap_util->write_srmap_of_data(new_lpa, dir_ino, blkoff);
        }
    }

    /* 回写SRMAP表并清空SRMAP缓存 */
    srmap_util->write_dirty_srmap_sync();
    srmap_util->clear_cache();
    
    syn.wait_cplt();  /* 等待所有异步写入完成 */

    /* 分配事务号 */
    std::unique_ptr<journal_container> cur_journal = fs_manager->get_and_reset_cur_journal();
    journal_process_env *journal_proc_env = journal_process_env::get_instance();
    uint64_t tx_id = journal_proc_env->alloc_tx_id();
    cur_journal->set_tx_id(tx_id);

    /* 构造淘汰保护信息并将其交给系统维护 */
    std::vector<dentry_handle> dirty_dentrys = fs_manager->get_dentry_cache()->get_and_clear_dirty_list();
    super_manager *sp_manager = fs_manager->get_super_manager();
    std::vector<uint32_t> uncommit_node_segs = sp_manager->get_and_clear_uncommit_node_segs();
    std::vector<uint32_t> uncommit_data_segs = sp_manager->get_and_clear_uncommit_data_segs();
    journal_container *rawp_cur_journal = cur_journal.get();  /* 保留raw pointer便于接下来提交日志 */

    transaction_replace_protect_record rp_record(tx_id, std::move(dirty_nodes), std::move(dirty_dentrys), std::move(cur_journal), 
        std::move(uncommit_node_segs), std::move(uncommit_data_segs));
    replace_protect_manager *rp_manager = fs_manager->get_replace_protect_manager();
    rp_manager->add_tx(std::move(rp_record));

    journal_proc_env->commit_journal(rawp_cur_journal);  /* 提交日志 */
}

} // namespace hscfs
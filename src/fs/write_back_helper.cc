#include "cache/block_buffer.hh"
#include "fs/write_back_helper.hh"
#include "fs/fs_manager.hh"
#include "fs/SIT_utils.hh"
#include "fs/super_manager.hh"
#include "fs/fs.h"

namespace hscfs {

write_back_helper::write_back_helper(file_system_manager *fs_manager)
    : super(fs_manager), sit_operator(fs_manager)
{
    this->fs_manager = fs_manager;
}

uint32_t write_back_helper::do_write_back_async(block_buffer &buffer, uint32_t &lpa, block_type type,
                                                comm_async_cb_func cb_func, void *cb_arg)
{
    uint32_t new_lpa;
    if (type == block_type::data)
        new_lpa = super.alloc_data_lpa();
    else
        new_lpa = super.alloc_node_lpa();
    
    if (lpa != INVALID_LPA)
        sit_operator.invalidate_lpa(lpa);
    
    lpa = new_lpa;
    buffer.write_to_lpa_async(fs_manager->get_device(), lpa, cb_func, cb_arg);

    return lpa;
}

} // namespace hscfs
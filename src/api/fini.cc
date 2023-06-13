#include "fs/fs_manager.hh"
#include "journal/journal_process_env.hh"
#include "communication/session.h"
#include "utils/hscfs_log.h"
#include "spdk/env.h"
#include "spdk/nvme.h"

namespace hscfs {

struct Device_Env
{
    spdk_nvme_transport_id trid;
    comm_dev dev;
};

extern Device_Env device_env;

void fini()
{
    try 
    {
        file_system_manager::fini();
        journal_process_env::get_instance()->stop_process_thread();
        comm_session_env_fini();
        comm_channel_controller_destructor(&device_env.dev.channel_ctrlr);
        spdk_nvme_detach(device_env.dev.nvme_ctrlr);
        spdk_env_fini();
    }
    catch (const std::exception &e)
    {
        HSCFS_LOG(HSCFS_LOG_ERROR, "exception occurred in fini: %s", e.what());
    }
}

}  // namespace hscfs

#ifdef CONFIG_C_API

extern "C" void hscfs_fini()
{
    hscfs::fini();
}

#endif
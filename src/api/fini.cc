#include "fs/fs_manager.hh"
#include "journal/journal_process_env.hh"
#include "communication/session.h"
#include "utils/hscfs_log.h"
#include "spdk/env.h"

namespace hscfs {

void fini()
{
    try 
    {
        file_system_manager::get_instance()->fini();
        journal_process_env::get_instance()->stop_process_thread();
        comm_session_env_fini();
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
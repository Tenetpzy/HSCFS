#include "spdk/env.h"
#include "spdk/nvme.h"
#include "communication/dev.h"
#include "communication/channel.h"
#include "communication/session.h"
#include "communication/memory.h"
#include "journal/journal_process_env.hh"
#include "fs/fs_manager.hh"
#include "fs/fs.h"
#include "cache/super_cache.hh"
#include "utils/hscfs_log.h"

#include <string>
#include <cstring>
#include <sys/sysinfo.h>

namespace hscfs {

#define CHANNEL_NUM_DEFAULT 4
#define TRID_CONFIG_PREFIX "--trid"

struct Device_Env
{
    spdk_nvme_transport_id trid;
    comm_dev dev;
};

Device_Env device_env;

/* 初始化device_env结构体中非spdk的成员，在初始化SPDK完成后调用 */
int device_env_init()
{
    int cpu_num = get_nprocs_conf();
    if (cpu_num <= 0)
    {
        HSCFS_LOG(HSCFS_LOG_WARNING, "failed to get cpu number.");
        cpu_num = CHANNEL_NUM_DEFAULT;
    }
    else
    {
        HSCFS_LOG(HSCFS_LOG_INFO, "detected cpu number: %d.", cpu_num);
    }

    if (comm_channel_controller_constructor(&device_env.dev.channel_ctrlr, &device_env.dev, cpu_num) != 0)
    {
        HSCFS_LOG(HSCFS_LOG_ERROR, "channel controller construct failed.");
        return -1;
    }
    return 0;
}

std::string parse_trid_from_argv(int argc, char *argv[])
{
    const std::string prefix = TRID_CONFIG_PREFIX;
    std::string trid;
    if (argc <= 1)
        return trid;
    for (int i = 1; i < argc; ++i)
    {
        if (std::strlen(argv[i]) < prefix.length())
            continue;
        if (std::string(argv[i], prefix.length()) != prefix)
            continue;
        trid = std::string(argv[i] + prefix.length(), argv[i] + std::strlen(argv[i]));
        HSCFS_LOG(HSCFS_LOG_INFO, "setting trid to %s.", trid.c_str());
        break;
    }
    return trid;
}

bool probe_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid, struct spdk_nvme_ctrlr_opts *opts)
{
	HSCFS_LOG(HSCFS_LOG_INFO, "Attaching to %s\n", trid->traddr);
	return true;
}

void attach_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
	  struct spdk_nvme_ctrlr *_ctrlr, const struct spdk_nvme_ctrlr_opts *opts)
{
    Device_Env *device_env = static_cast<Device_Env*>(cb_ctx);
    int nsid = spdk_nvme_ctrlr_get_first_active_ns(_ctrlr);
    if (nsid == 0)
    {
        HSCFS_LOG(HSCFS_LOG_ERROR, "no active ns detected for %s", trid->traddr);
        return;
    }
    device_env->dev.ns = spdk_nvme_ctrlr_get_ns(_ctrlr, nsid);
    if (device_env->dev.ns == NULL)
    {
        HSCFS_LOG(HSCFS_LOG_ERROR, "failed to get ns handler");
        return;
    }
    device_env->dev.nvme_ctrlr = _ctrlr;
}

/* 初始化SPDK，并初始化device_env中与SPDK有关的成员 */
int spdk_init(const std::string &trid)
{
    spdk_env_opts opts;
    spdk_env_opts_init(&opts);
    opts.name = "hscfs";
	if (spdk_env_init(&opts) < 0) 
    {
		HSCFS_LOG(HSCFS_LOG_ERROR, "Unable to initialize SPDK env");
		return -1;
	}

	HSCFS_LOG(HSCFS_LOG_INFO, "Initializing NVMe Controllers");

    spdk_nvme_trid_populate_transport(&device_env.trid, SPDK_NVME_TRANSPORT_PCIE);
    std::memcpy(device_env.trid.traddr, trid.c_str(), trid.length());

    int rc = spdk_nvme_probe(&device_env.trid, &device_env, probe_cb, attach_cb, NULL);
	if (rc != 0) 
    {
		HSCFS_LOG(HSCFS_LOG_ERROR, "spdk_nvme_probe() failed");
		return -1;
	}
    if (device_env.dev.nvme_ctrlr == NULL)
    {
        HSCFS_LOG(HSCFS_LOG_ERROR, "No device detected!");
        return -1;
    }
    if (device_env.dev.ns == NULL)
    {
        HSCFS_LOG(HSCFS_LOG_ERROR, "No namespace detected!");
        return -1;
    }

    HSCFS_LOG(HSCFS_LOG_INFO, "SPDK Initialization complete.");
    return 0;
}

/* SSD侧文件系统初始化：启动FS模块功能，使用DB回复超级块，启动元数据日志应用。在初始化device_env和初始化通信层完成后调用。 */
static int ssd_init()
{
    comm_dev *dev = &device_env.dev;

    /* SSD侧FS模块初始化 */
    HSCFS_LOG(HSCFS_LOG_INFO, "Initializing SSD fs module...");
    if (comm_submit_fs_module_init_request(dev) != 0)
    {
        HSCFS_LOG(HSCFS_LOG_ERROR, "SSD fs module init failed.");
        return -1;
    }

    /* 
     * 已知缺陷：FS模块初始化是异步的（SSD侧异步读取Super Block），还没有执行完就会向主机侧返回CQE，
     * 执行完后不会通知主机，主机也无法主动轮询该命令执行状态。
     * 所以，后续依赖SSD DRAM中Super Block缓存的命令会执行失败，甚至访问到未初始化的指针。
     * 目前通过主机侧等待足够长的时间来解决此问题
     */
    HSCFS_LOG(HSCFS_LOG_INFO, "Waiting SSD fs module init complete...");
    sleep(3);

    /* 恢复文件系统超级块 */
    HSCFS_LOG(HSCFS_LOG_INFO, "Recovering SSD super block...");
    if (comm_submit_fs_recover_from_db_request(dev) != 0)
    {
        HSCFS_LOG(HSCFS_LOG_ERROR, "recover SSD super block failed.");
        return -1;
    }

    /* 启动元数据日志应用任务 */
    HSCFS_LOG(HSCFS_LOG_INFO, "Start SSD journal processor.");
    if (comm_submit_start_apply_journal_request(dev) != 0)
    {
        HSCFS_LOG(HSCFS_LOG_ERROR, "start SSD journal processor failed.");
        return -1;
    }

    return 0;
}

/* 
 * 进行SSD故障恢复。如果SSD有未处理的日志，等待日志处理完成
 * 将故障恢复后，SSD Meta Journal FIFO区域的头指针(与尾指针相等)记录到参数journal_fifo_pos中
 * 在ssd_init后调用
 */
static int ssd_recovery(uint64_t &journal_fifo_pos)
{   
    /* 分配用于保存Journal FIFO头尾指针的缓存 */
    uint64_t *journal_fifo_buffer = static_cast<uint64_t*>(comm_alloc_dma_mem(16));
    if (journal_fifo_buffer == NULL)
        return -1;

    HSCFS_LOG(HSCFS_LOG_INFO, "Waiting SSD journal recovery...");
    do
    {
        if (comm_submit_sync_get_metajournal_head_request(&device_env.dev, journal_fifo_buffer) != 0)
        {
            HSCFS_LOG(HSCFS_LOG_ERROR, "failed to get SSD meta journal fifo pos.");
            comm_free_dma_mem(journal_fifo_buffer);
            return -1;
        }
    } while (journal_fifo_buffer[0] != journal_fifo_buffer[1]);
    
    HSCFS_LOG(HSCFS_LOG_INFO, "SSD journal recovery complete, current journal fifo pos = %lu.", journal_fifo_buffer[0]);
    journal_fifo_pos = journal_fifo_buffer[0];
    comm_free_dma_mem(journal_fifo_buffer);
    return 0;
}

int init(int argc, char *argv[])
{
    try
    {
        /* 初始化SPDK与通信层使用的环境 */
        HSCFS_LOG(HSCFS_LOG_INFO, "Initializing SPDK and communication layer...");
        std::string trid = parse_trid_from_argv(argc, argv);
        if (trid.empty())
        {
            HSCFS_LOG(HSCFS_LOG_ERROR, "could not find trid of device!");
            return -1;
        }

        if (spdk_init(trid) != 0)
            return -1;
        
        if (device_env_init() != 0)
            return -1;

        /* 初始化会话层并启动会话层轮询线程 */
        if (comm_session_env_init(&device_env.dev) != 0)
            return -1;
        
        /* TODO: 检查文件系统魔数 */

        /* 初始化SSD侧文件系统模块，启动SSD日志执行 */
        if (ssd_init() != 0)
            return -1;
        
        /* 等待故障恢复 */
        uint64_t journal_fifo_pos;
        if (ssd_recovery(journal_fifo_pos) != 0)
            return -1;

        /* 初始化文件系统层 */
        HSCFS_LOG(HSCFS_LOG_INFO, "Initializing file system layer...");
        file_system_manager::init(&device_env.dev);

        /* 初始化日志管理层 */
        HSCFS_LOG(HSCFS_LOG_INFO, "Initializing journal layer...");
        super_cache &super = *(file_system_manager::get_instance()->get_super_cache());
        journal_process_env::get_instance()->init(&device_env.dev, super->meta_journal_start_blkoff, super->meta_journal_end_blkoff, 
            journal_fifo_pos);
        
        HSCFS_LOG(HSCFS_LOG_INFO, "HSCFS initialization complete.");
        return 0;
    }
    catch (const std::exception &e)
    {
        HSCFS_LOG(HSCFS_LOG_ERROR, "exception occurred when initialization: %s", e.what());
        return -1;
    }
}

}  // namespace hscfs

#ifdef CONFIG_C_API

extern "C" int hscfs_init(int argc, char *argv[])
{
    return hscfs::init(argc, argv);
}

#endif
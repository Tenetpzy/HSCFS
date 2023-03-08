#include "communication/dev.h"
#include "communication/comm_api.h"
#include "communication/session.h"
#include "spdk/nvme.h"

#include "gtest/gtest.h"

#include <stdexcept>
#include <thread>
#include <cstdio>

comm_dev dev;
std::thread th;
polling_thread_start_env polling_thread_arg;

struct spdk_nvme_transport_id g_trid;
const size_t test_channel_size = 8;
const uint64_t super_lba = 0;

// 测试的日志起止LPA
const uint64_t test_journal_start_lpa = 1;
const uint64_t test_journal_lpa_num = 5;
const uint64_t test_journal_end_lpa = 6; 

bool probe_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid, struct spdk_nvme_ctrlr_opts *opts)
{
	printf("Attaching to %s\n", trid->traddr);
	return true;
}

void attach_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
	  struct spdk_nvme_ctrlr *_ctrlr, const struct spdk_nvme_ctrlr_opts *opts)
{
    int nsid = spdk_nvme_ctrlr_get_first_active_ns(_ctrlr);
    if (nsid == 0)
    {
        printf("no active ns detected for %s\n", trid->traddr);
        exit(1);
    }
    dev.ns = spdk_nvme_ctrlr_get_ns(_ctrlr, nsid);
    if (dev.ns == NULL)
    {
        printf("failed to get ns handler\n");
        exit(1);
    }
    dev.nvme_ctrlr = _ctrlr;
}

void init_spdk(void)
{
    struct spdk_env_opts opts;
	spdk_env_opts_init(&opts);
	opts.name = "hello_world";
	if (spdk_env_init(&opts) < 0) 
    {
		printf("Unable to initialize SPDK env\n");
		exit(1);
	}
	printf("Initializing NVMe Controllers\n");

    spdk_nvme_trid_populate_transport(&g_trid, SPDK_NVME_TRANSPORT_PCIE);
    printf("input device pcie address in the format DDDD:BB:DD.FF:\n");
    fflush(stdout);
    scanf("%s", &g_trid.traddr[0]);

    int rc = spdk_nvme_probe(&g_trid, NULL, probe_cb, attach_cb, NULL);
	if (rc != 0) 
    {
		printf("spdk_nvme_probe() failed\n");
		exit(1);
	}
    if (dev.nvme_ctrlr == NULL)
    {
        printf("No device detected!\n");
        exit(1);
    }
    printf("Initialization complete.\n");
}

void host_env_init(void)
{
    init_spdk();
    if (comm_channel_controller_constructor(&dev.channel_ctrlr, &dev, test_channel_size) != 0)
        throw std::runtime_error("channel controller construct failed.");
    if (comm_session_env_constructor() != 0)
        throw std::runtime_error("session env init failed.");
    polling_thread_arg.dev = &dev;
    th = std::thread(comm_session_polling_thread, static_cast<void*>(&polling_thread_arg));
    th.detach();
}

void SSD_test_prepare(void)
{
    f2fs_spuer_block super_block;
    super_block.meta_journal_start_blkoff = test_journal_start_lpa;
    super_block.meta_journal_end_blkoff = test_journal_end_lpa;

    // 写入super block

    // 文件系统模块初始化（DRAM中缓存SB）

    // DB区域初始化（DB中存储SB）

    // 清空元数据日志（DB中日志首尾指针指向SB中的日志起始LPA）

}

// 测试：
/*
 * 写入若干END日志项，一个LPA写一个
 * 启动元数据日志应用
 * 轮询读取元数据首位置
 */
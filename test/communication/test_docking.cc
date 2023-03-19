#include "communication/dev.h"
#include "communication/comm_api.h"
#include "communication/session.h"
#include "communication/memory.h"
#include "journal/journal_type.h"
#include "spdk/nvme.h"

// #include "spdk_mock.h"  // for local test

#include <stdexcept>
#include <thread>
#include <cstdio>
#include <chrono>

comm_dev dev;
std::thread th;
polling_thread_start_env polling_thread_arg;

struct spdk_nvme_transport_id g_trid;
const size_t test_channel_size = 8;
const uint64_t super_lpa = 0;
const size_t lpa_size = 4096;

// 测试的日志起止LPA
const uint64_t test_journal_start_lpa = 1;
const uint64_t test_journal_lpa_num = 3;
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
    printf("SPDK initialization complete.\n");
}

void host_env_init(void)
{
    init_spdk();
    // spdk_stub_setup();  // for local test

    if (comm_channel_controller_constructor(&dev.channel_ctrlr, &dev, test_channel_size) != 0)
        throw std::runtime_error("channel controller construct failed.");
    if (comm_session_env_init(&dev) != 0)
        throw std::runtime_error("session env init failed.");
}

void SSD_test_prepare(void)
{
    f2fs_super_block super = {0};
    super.meta_journal_start_blkoff = test_journal_start_lpa;
    super.meta_journal_end_blkoff = test_journal_end_lpa;

    // 写入super block
    char *super_block = static_cast<char*>(comm_alloc_dma_mem(lpa_size));
    if (super_block == NULL)
        throw std::runtime_error("alloc super block memory failed.");
    memcpy(super_block, &super, sizeof(f2fs_super_block));
    printf("super block meta_journal_start_blkoff: %hu, end: %hu\n", 
        reinterpret_cast<f2fs_super_block*>(super_block)->meta_journal_start_blkoff, 
        reinterpret_cast<f2fs_super_block*>(super_block)->meta_journal_end_blkoff);
    if (comm_submit_sync_rw_request(&dev, super_block, super_lpa, 8, COMM_IO_WRITE) != 0)
        throw std::runtime_error("write super block failed.");
    comm_free_dma_mem(super_block);

    // 文件系统模块初始化（DRAM中缓存SB）
    if (comm_submit_fs_module_init_request(&dev) != 0)
        throw std::runtime_error("fs module init failed.");

    // DB区域初始化（DB中存储SB）
    if (comm_submit_fs_db_init_request(&dev) != 0)
        throw std::runtime_error("fs db init failed.");

    // 清空元数据日志（DB中日志首尾指针指向SB中的日志区域起始LPA）
    if (comm_submit_clear_metajournal_request(&dev) != 0)
        throw std::runtime_error("clear journal failed.");
    
    // 启动元数据日志应用任务
    if (comm_submit_start_apply_journal_request(&dev) != 0)
        throw std::runtime_error("start journal apply failed.");
}

// 测试：
/*
 * 写入若干END日志项，一个LPA写一个
 * 更新SSD日志尾指针
 * 轮询读取元数据首位置，直到元数据应用完成
 */
void test_func(void)
{
    using namespace std::chrono_literals;

    uint64_t endlpa = test_journal_start_lpa + test_journal_lpa_num;
    meta_journal_entry end_entry = {.len = sizeof(meta_journal_entry), .type = JOURNAL_TYPE_END};
    char *block_buffer = static_cast<char*>(comm_alloc_dma_mem(lpa_size));
    if (block_buffer == NULL)
        throw std::runtime_error("alloc journal buffer failed.");
    memcpy(block_buffer, &end_entry, sizeof(meta_journal_entry));
    printf("journal block buffer .type = %hhu\n", reinterpret_cast<meta_journal_entry*>(block_buffer)->type);
    for (uint64_t tarlpa = test_journal_start_lpa; tarlpa < endlpa; ++tarlpa)
    {
        if (comm_submit_sync_rw_request(&dev, block_buffer, tarlpa * 8, 8, COMM_IO_WRITE) != 0)
            throw std::runtime_error("write test journal failed.");
    }

    if (comm_submit_sync_update_metajournal_tail_request(&dev, test_journal_start_lpa, test_journal_lpa_num) != 0)
        throw std::runtime_error("update journal tail failed.");

    uint64_t *journal_head = static_cast<uint64_t*>(comm_alloc_dma_mem(8));
    if (journal_head == NULL)
        throw std::runtime_error("alloc journal head buffer failed.");
    
    size_t req_cnt = 0;
    while (true)
    {
        std::this_thread::sleep_for(500ms);
        printf("request journal head for %lu times...\n", ++req_cnt);
        if (comm_submit_sync_get_metajournal_head_request(&dev, journal_head) != 0)
            throw std::runtime_error("get journal head failed.");
        printf("journal head: %lu\n", *journal_head);
        if (*journal_head == endlpa)
            break;
    }

    comm_free_dma_mem(journal_head);
    comm_free_dma_mem(block_buffer);
}

int main()
{
    host_env_init();
    SSD_test_prepare();
    test_func();
    return 0;
}
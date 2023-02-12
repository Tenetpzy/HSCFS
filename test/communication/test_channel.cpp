#include "stub.h"
#include "gtest/gtest.h"

#include <unordered_map>
#include <vector>
#include <mutex>

#include "../../src/communication/channel.c"

struct spdk_cmd_cb
{
    spdk_nvme_cmd_cb _cb_fn;
    void *_cb_arg;

    spdk_cmd_cb(spdk_nvme_cmd_cb cb_fn, void * cb_arg) : _cb_fn(cb_fn), _cb_arg(cb_arg) {}
};

static const size_t test_channel_size = 8;
static struct spdk_nvme_qpair *qpair_addr_stub[test_channel_size];
static std::vector<spdk_cmd_cb> qpair_cmds[test_channel_size];

static const size_t lba_size = 512;
using lba_buffer = char[lba_size];
static std::unordered_map<uint64_t, lba_buffer> vir_lba_storage;
static std::mutex storage_mtx;

static inline size_t qpair_addr_to_idx(struct spdk_nvme_qpair *qpair)
{
    return (size_t)qpair;
};

struct spdk_nvme_qpair *spdk_nvme_ctrlr_alloc_io_qpair(struct spdk_nvme_ctrlr *ctrlr,
		const struct spdk_nvme_io_qpair_opts *opts,
		size_t opts_size)
{
    return (struct spdk_nvme_qpair *)malloc(sizeof(char));
}

int spdk_nvme_ctrlr_free_io_qpair(struct spdk_nvme_qpair *qpair) 
{
    return 0;
}

const char *spdk_nvme_cpl_get_status_string(const struct spdk_nvme_status *status) {return NULL;}

int spdk_nvme_ns_cmd_read(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair, void *payload,
			  uint64_t lba, uint32_t lba_count, spdk_nvme_cmd_cb cb_fn,
			  void *cb_arg, uint32_t io_flags)
{
    {
        std::lock_guard<std::mutex> lg(storage_mtx);
        for (uint32_t cnt = 0; cnt < lba_count; ++cnt)
            memcpy((char*)payload + cnt * lba_size, vir_lba_storage[lba + cnt], lba_size);
    }
    qpair_cmds[qpair_addr_to_idx(qpair)].emplace_back(cb_fn, cb_arg);
    return 0;
}

int spdk_nvme_ns_cmd_write(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair, void *payload,
			   uint64_t lba, uint32_t lba_count, spdk_nvme_cmd_cb cb_fn,
			   void *cb_arg, uint32_t io_flags)
{
    {
        std::lock_guard<std::mutex> lg(storage_mtx);
        for (uint32_t cnt = 0; cnt < lba_count; ++cnt)
            memcpy(vir_lba_storage[lba + cnt], (char*)payload + cnt * lba_size, lba_size);
    }
    qpair_cmds[qpair_addr_to_idx(qpair)].emplace_back(cb_fn, cb_arg);
    return 0;
}

int spdk_nvme_ctrlr_cmd_admin_raw(struct spdk_nvme_ctrlr *ctrlr,
				  struct spdk_nvme_cmd *cmd,
				  void *buf, uint32_t len,
				  spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
    
}

class channel_test: public ::testing::Test
{
public:



    void SetUp() override
    {
        
    }

    void TearDown() override
    {

    }
};

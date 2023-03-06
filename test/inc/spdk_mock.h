#include <cstring>
#include <unordered_map>
#include <vector>
#include <mutex>

#ifdef __cplusplus
extern "C" {
#endif

#include "spdk/nvme.h"
#include "spdk/nvme_spec.h"

struct spdk_cmd_cb
{
    spdk_nvme_cmd_cb _cb_fn;
    void *_cb_arg;
	uint16_t _tid;

    spdk_cmd_cb(uint16_t tid, spdk_nvme_cmd_cb cb_fn, void * cb_arg) : _tid(tid), _cb_fn(cb_fn), _cb_arg(cb_arg) {}
};

struct spdk_nvme_ctrlr{};
struct spdk_nvme_ns{};

static const size_t test_channel_size = 8;
static const size_t lba_size = 512;
static const size_t max_ret_tid = 16;

void spdk_stub_setup(void);

// 使用index + 1数值模拟指针
struct spdk_nvme_qpair *spdk_nvme_ctrlr_alloc_io_qpair(struct spdk_nvme_ctrlr *ctrlr,
		const struct spdk_nvme_io_qpair_opts *opts,
		size_t opts_size);

int spdk_nvme_ctrlr_free_io_qpair(struct spdk_nvme_qpair *qpair);

#undef spdk_nvme_cpl_is_error
#define spdk_nvme_cpl_is_error(x) false

const char *spdk_nvme_cpl_get_status_string(const struct spdk_nvme_status *status);

uint32_t spdk_nvme_ns_get_id(spdk_nvme_ns *ns);

int spdk_nvme_ns_cmd_read(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair, void *payload,
			  uint64_t lba, uint32_t lba_count, spdk_nvme_cmd_cb cb_fn,
			  void *cb_arg, uint32_t io_flags);

int spdk_nvme_ns_cmd_write(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair, void *payload,
			   uint64_t lba, uint32_t lba_count, spdk_nvme_cmd_cb cb_fn,
			   void *cb_arg, uint32_t io_flags);

// stub测试不要依赖buf和len参数
int spdk_nvme_ctrlr_cmd_admin_raw(struct spdk_nvme_ctrlr *ctrlr,
				  struct spdk_nvme_cmd *cmd,
				  void *buf, uint32_t len,
				  spdk_nvme_cmd_cb cb_fn, void *cb_arg);

int32_t spdk_nvme_qpair_process_completions(struct spdk_nvme_qpair *qpair,
		uint32_t max_completions);

int32_t spdk_nvme_ctrlr_process_admin_completions(spdk_nvme_ctrlr *ctrlr);

void *spdk_zmalloc(size_t size, size_t align, uint64_t *phys_addr, int socket_id, uint32_t flags);

void spdk_free(void *buf);

#ifdef __cplusplus
}
#endif
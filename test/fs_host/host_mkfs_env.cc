#include "spdk/nvme.h"
#include "communication/comm_api.h"
#include "communication/session.h"
#include "communication/channel.h"

#include <stdexcept>

extern int fd;  // host_test_env中的fd

namespace hscfs {

struct Device_Env
{
    spdk_nvme_transport_id trid;
    comm_dev dev;
};

extern Device_Env device_env;

}

extern "C" {

#define MOCK_SSD_BLOCK_SIZE (1024U * 1024 * 1024 * 2 / 512U)

/* 若主机侧mock mkhscfs，则创建包含1024 / 8 = 128 个块的模拟SSD */
uint64_t spdk_nvme_ns_get_num_sectors(struct spdk_nvme_ns *ns)
{
    return MOCK_SSD_BLOCK_SIZE;
}

/* 主机侧mock mkhscfs，此时打开文件fd */
int comm_session_env_init(comm_dev *dev)
{
    fd = open("./fsImage", O_RDWR | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR);
    if (fd == -1)
        throw std::runtime_error("open fsImage error");
    return 0;
}

int comm_submit_fs_module_init_request(comm_dev *dev) { return 0; }

int comm_submit_fs_recover_from_db_request(comm_dev *dev) { return 0; }

int comm_submit_start_apply_journal_request(comm_dev *dev)  { return 0; }

int comm_submit_fs_db_init_request(comm_dev *dev) { return 0; }

int comm_submit_clear_metajournal_request(comm_dev *dev) { return 0; }

int comm_channel_controller_constructor(comm_channel_controller *self, comm_dev *dev, size_t channel_num) { return 0; }

uint32_t spdk_nvme_ctrlr_get_first_active_ns(struct spdk_nvme_ctrlr *ctrlr) {return 1; }

struct spdk_nvme_ns *spdk_nvme_ctrlr_get_ns(struct spdk_nvme_ctrlr *ctrlr, uint32_t ns_id) { return (spdk_nvme_ns*)8; }

void spdk_env_opts_init(struct spdk_env_opts *opts) {}

int spdk_env_init(const struct spdk_env_opts *opts) { return 0; }

void spdk_nvme_trid_populate_transport(struct spdk_nvme_transport_id *trid,
				       enum spdk_nvme_transport_type trtype)  {}

int spdk_nvme_probe(const struct spdk_nvme_transport_id *trid,
		    void *cb_ctx,
		    spdk_nvme_probe_cb probe_cb,
		    spdk_nvme_attach_cb attach_cb,
		    spdk_nvme_remove_cb remove_cb)
{
    hscfs::device_env.dev.nvme_ctrlr = (spdk_nvme_ctrlr*)1;
    hscfs::device_env.dev.ns = (spdk_nvme_ns*)1;
    return 0;
}


}
#include <cstring>
#include <unordered_map>
#include <vector>
#include <queue>
#include <mutex>
#include <array>

#include "spdk_mock.h"

extern "C" {

static const uint16_t INVALID_TID = 0;

static int qpair_stub_alloc_map[test_channel_size];
static std::vector<spdk_cmd_cb> qpair_io_cmds[test_channel_size], qpair_admin_cmds;
static std::mutex admin_mtx, io_mtx;

spdk_nvme_cpl cpl_stub;
std::queue<spdk_cmd_cb> cplt_tid_cmd;
uint16_t ret_tid_list[max_ret_tid];

class lba_buffer
{
public:
    char *get_ptr()
    {
        return buf;
    }
private:
    char buf[lba_size];
};
static std::unordered_map<uint64_t, lba_buffer> vir_lba_storage;

void spdk_stub_setup(void)
{
    for (size_t i = 0; i < test_channel_size; ++i)
    {
        qpair_stub_alloc_map[i] = 0;
    }
}

static size_t qpair_addr_to_idx(struct spdk_nvme_qpair *qpair)
{
    return reinterpret_cast<size_t>(qpair) - 1;
};

// 使用index + 1数值模拟指针
struct spdk_nvme_qpair *spdk_nvme_ctrlr_alloc_io_qpair(struct spdk_nvme_ctrlr *ctrlr,
		const struct spdk_nvme_io_qpair_opts *opts,
		size_t opts_size)
{
    size_t res;
    for (res = 0; res < test_channel_size; ++res)
    {
        if (qpair_stub_alloc_map[res] == 0)
        {
            qpair_stub_alloc_map[res] = 1;
            break;
        }
    }
    if (res == test_channel_size)
        return NULL;
    return reinterpret_cast<struct spdk_nvme_qpair *>(res + 1);
}

int spdk_nvme_ctrlr_free_io_qpair(struct spdk_nvme_qpair *qpair) 
{
    size_t index = qpair_addr_to_idx(qpair);
    qpair_stub_alloc_map[index] = 0;
    return 0;
}

#undef spdk_nvme_cpl_is_error
#define spdk_nvme_cpl_is_error(x) false

const char *spdk_nvme_cpl_get_status_string(const struct spdk_nvme_status *status) {return NULL;}

uint32_t spdk_nvme_ns_get_id(spdk_nvme_ns *ns)
{
    return 0;
}

int spdk_nvme_ns_cmd_read(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair, void *payload,
			  uint64_t lba, uint32_t lba_count, spdk_nvme_cmd_cb cb_fn,
			  void *cb_arg, uint32_t io_flags)
{
    std::lock_guard<std::mutex> lg(io_mtx);
    for (uint32_t cnt = 0; cnt < lba_count; ++cnt)
    {
        if (vir_lba_storage.count(lba + cnt))
            memcpy(static_cast<char*>(payload) + cnt * lba_size, vir_lba_storage[lba + cnt].get_ptr(), lba_size);
        else
            memcpy(static_cast<char*>(payload) + cnt * lba_size, "empty block.", sizeof("empty block."));
    }
    qpair_io_cmds[qpair_addr_to_idx(qpair)].emplace_back(INVALID_TID, cb_fn, cb_arg);
    return 0;
}

int spdk_nvme_ns_cmd_write(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair, void *payload,
			   uint64_t lba, uint32_t lba_count, spdk_nvme_cmd_cb cb_fn,
			   void *cb_arg, uint32_t io_flags)
{
    std::lock_guard<std::mutex> lg(io_mtx);
    for (uint32_t cnt = 0; cnt < lba_count; ++cnt)
        memcpy(vir_lba_storage[lba + cnt].get_ptr(), static_cast<char*>(payload) + cnt * lba_size, lba_size);
    qpair_io_cmds[qpair_addr_to_idx(qpair)].emplace_back(INVALID_TID, cb_fn, cb_arg);
    return 0;
}

int spdk_nvme_ctrlr_cmd_admin_raw(struct spdk_nvme_ctrlr *ctrlr,
				  struct spdk_nvme_cmd *cmd,
				  void *buf, uint32_t len,
				  spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
    auto is_long_cmd = [](struct spdk_nvme_cmd *cmd) {
        return cmd->cdw12 == 0x10021 || cmd->cdw12 == 0x20021 || cmd->cdw12 == 0x30021;
    };

    std::lock_guard<std::mutex> lg(admin_mtx);

    if (cmd->cdw12 == 0x50021)  // 获取已完成tid列表
    {
        size_t request_cnt = static_cast<size_t>(len) / sizeof(uint32_t);
        size_t cnt = std::min(request_cnt, max_ret_tid);
        cnt = std::min(cnt, cplt_tid_cmd.size());
        size_t i = 0;
        for (; i < cnt; ++i)
        {
            auto &e = cplt_tid_cmd.front();
            ret_tid_list[i] = e._tid;
            cplt_tid_cmd.pop();
        }
        for (; i < request_cnt; ++i)
            ret_tid_list[i] = INVALID_TID;
        memcpy(buf, ret_tid_list, len);
    }

    else if (cmd->cdw12 == 0x60021) // 获取tid对应任务结果
    {
        static char tid_cmd_res[] = "tid test result";
        memcpy(buf, tid_cmd_res, std::min(static_cast<size_t>(len), sizeof(tid_cmd_res)));
    }

    uint16_t tid = is_long_cmd(cmd) ? cmd->cdw13 : INVALID_TID;
    qpair_admin_cmds.emplace_back(tid, cb_fn, cb_arg);
    return 0;
}

int32_t spdk_nvme_qpair_process_completions(struct spdk_nvme_qpair *qpair,
		uint32_t max_completions)
{
    std::lock_guard<std::mutex> lg(io_mtx);
    size_t index = qpair_addr_to_idx(qpair);
    std::vector<spdk_cmd_cb> &qpair_cmds = qpair_io_cmds[index];
    size_t cpl_cnt = max_completions == 0 ? qpair_cmds.size() : max_completions;
    for (size_t i = 0; i < cpl_cnt; ++i)
    {
        spdk_cmd_cb &ctx = qpair_cmds[i];
        ctx._cb_fn(ctx._cb_arg, &cpl_stub);
    }
    qpair_cmds.erase(qpair_cmds.begin(), qpair_cmds.begin() + cpl_cnt);
    return static_cast<int32_t>(cpl_cnt);
}

int32_t spdk_nvme_ctrlr_process_admin_completions(spdk_nvme_ctrlr *ctrlr)
{
    std::lock_guard<std::mutex> lg(admin_mtx);
    size_t cpl_cnt = qpair_admin_cmds.size();
    for (size_t i = 0; i < cpl_cnt; ++i)
    {
        spdk_cmd_cb &ctx = qpair_admin_cmds[i];
        ctx._cb_fn(ctx._cb_arg, &cpl_stub);

        if (ctx._tid != INVALID_TID)
            cplt_tid_cmd.emplace(ctx);
    }
    qpair_admin_cmds.clear();
    return static_cast<int32_t>(cpl_cnt);
}

void *spdk_zmalloc(size_t size, size_t align, uint64_t *phys_addr, int socket_id, uint32_t flags)
{
    return malloc(size);
}

void spdk_free(void *buf)
{
    free(buf);
}

} // extern "C"
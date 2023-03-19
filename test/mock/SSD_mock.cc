#include "SSD_mock.hh"
#include <thread>

SSD_device_mock::SSD_device_mock(size_t lba_num, size_t io_qpair_size)
{
    flash.resize(lba_num, {0});
    io_qpairs.resize(io_qpair_size);
    total_pending_sq_cmds = 0;
}

void SSD_device_mock::submit_io_cmd(spdk_nvme_qpair *qpair, const SSD_cmd_info &cmd)
{
    size_t idx = reinterpret_cast<size_t>(qpair);
    bool need_wakeup = false;

    {
        std::lock_guard<std::mutex> lg(qpair_mtx);
        io_qpairs[idx].sq.emplace_back(cmd);
        need_wakeup = total_pending_sq_cmds == 0;
        ++total_pending_sq_cmds;
    }

    if (need_wakeup)
        sq_cond.notify_all();
}

void SSD_device_mock::submit_admin_cmd(const SSD_cmd_info &cmd)
{
    bool need_wakeup = false;

    {
        std::lock_guard<std::mutex> lg(qpair_mtx);
        admin_qpair.sq.emplace_back(cmd);
        need_wakeup = total_pending_sq_cmds == 0;
        ++total_pending_sq_cmds;
    }

    if (need_wakeup)
        sq_cond.notify_all();   
}

std::list<uint64_t> SSD_device_mock::get_qpair_cplt_cid_list(spdk_nvme_qpair *qpair, uint32_t max_cplt)
{
    std::list<uint64_t> res;
    size_t idx = reinterpret_cast<size_t>(qpair);
    std::lock_guard<std::mutex> lg(qpair_mtx);

    auto &cq = io_qpairs[idx].cq;
    size_t cnt = std::min(cq.size(), static_cast<size_t>(max_cplt));

    for (; cnt > 0; --cnt)
        res.splice(res.end(), cq, cq.begin());

    return res;
}

std::list<uint64_t> SSD_device_mock::get_admin_cplt_cid_list()
{
    std::list<uint64_t> res;
    std::lock_guard<std::mutex> lg(qpair_mtx);
    res.splice(res.end(), admin_qpair.cq);
    return res;
}

void SSD_device_mock::start_mock()
{
    std::thread mock_th(&SSD_device_mock::mock_thread, this);
    mock_th.detach();
}

void SSD_device_mock::mock_thread()
{
    
}

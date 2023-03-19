#include "communication/dev.h"
#include "communication/comm_api.h"
#include "communication/session.h"

#include "spdk_mock.h"
#include "gtest/gtest.h"

#include <stdexcept>
#include <thread>
#include <cstdio>

comm_dev dev;
std::thread th;
polling_thread_start_env polling_thread_arg;

TEST(comm_test, sync_read_single_thread)
{
    char buf[lba_size];
    if (comm_submit_sync_rw_request(&dev, buf, 0, 1, COMM_IO_READ) != 0)
        throw std::runtime_error("submit sync read request error.");
    
    fprintf(stdout, "%s\n", buf);
}

void sync_read_multiple_thread_test(size_t idx, comm_dev *dev)
{
    char buf[lba_size];
    if (comm_submit_sync_rw_request(dev, buf, idx, 1, COMM_IO_READ) != 0)
        throw std::runtime_error("submit sync read request error.");
    
    fprintf(stdout, "thread %lu: %s\n", idx, buf);
}

TEST(comm_test, sync_read_multiple_thread)
{
    const size_t th_num = 16;
    const size_t test_num = 1024;
    for (size_t i = 0; i < test_num; ++i)
    {
        fprintf(stdout, "test round: %lu\n", i);
        std::thread ths[th_num];
        for (size_t i = 0; i < th_num; ++i)
            ths[i] = std::thread(sync_read_multiple_thread_test, i, &dev);
        for (size_t i = 0; i < th_num; ++i)
            ths[i].join();
        std::cout << std::endl;
    }
}

void async_read_test_cb(comm_cmd_result res, void *arg)
{
    char *buf = static_cast<char*>(arg);
    fprintf(stdout, "%s\n", buf);
    delete[] buf;
}

void async_read_test_thread(size_t index)
{
    const size_t buf_size = 512;
    char *buffer = new char[buf_size];
    int ret = comm_submit_async_rw_request(&dev, buffer, 0, 1, async_read_test_cb, static_cast<void*>(buffer), COMM_IO_READ);
    if (ret != 0)
        throw std::runtime_error("send async read req failed.");
    fprintf(stdout, "thread %lu exit.\n", index);
}

TEST(comm_test, async_read_multi_thread)
{
    const size_t th_num = 16;
    const size_t test_num = 1024;
    for (size_t i = 0; i < test_num; ++i)
    {
        fprintf(stdout, "test round: %lu\n", i);
        std::thread ths[th_num];
        for (size_t i = 0; i < th_num; ++i)
            ths[i] = std::thread(async_read_test_thread, i);
        for (size_t i = 0; i < th_num; ++i)
            ths[i].join();
        fprintf(stdout, "\n");
    }
}

void sync_raw_long_cmd_test(size_t idx)
{
    const size_t cmd_res_len = 128;
    char cmd_result[cmd_res_len];
    filemapping_search_task task;
    if (comm_submit_sync_filemapping_search_request(&dev, &task, cmd_result, cmd_res_len) != 0)
        throw std::runtime_error("send filemapping search failed.");
    fprintf(stdout, "thread %lu: %s\n", idx, cmd_result);
}

TEST(comm_test, sync_raw_long_cmd_single)
{
    sync_raw_long_cmd_test(0);
}

TEST(comm_test, sync_raw_long_cmd_multi_thread)
{
    const size_t th_num = 16;
    const size_t test_num = 1024;
    for (size_t i = 0; i < test_num; ++i)
    {
        fprintf(stdout, "test round: %lu\n", i);
        std::thread ths[th_num];
        for (size_t i = 0; i < th_num; ++i)
            ths[i] = std::thread(sync_raw_long_cmd_test, i);
        for (size_t i = 0; i < th_num; ++i)
            ths[i].join();
        std::cout << std::endl;
    }
}

int main(int argc, char **argv)
{
    int ret = 0;
    ret = comm_channel_controller_constructor(&dev.channel_ctrlr, &dev, test_channel_size);
    if (ret != 0)
        throw std::runtime_error("channel controller construct failed.");
    if (comm_session_env_init(&dev) != 0)
        throw std::runtime_error("session env init failed.");
    spdk_stub_setup();

    ::testing::InitGoogleTest(&argc, argv);
    ret = RUN_ALL_TESTS();

    // comm_session_env_fini();
    // comm_channel_controller_destructor(&dev.channel_ctrlr);
    return ret;
}
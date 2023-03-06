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

// void comm_async_cb_func(comm_cmd_result res, void *arg)
// {

// }

// TEST_F(comm_test, sync_read_single_thread)
// {
//     char buf[lba_size];
//     if (comm_submit_sync_read_request(&dev, buf, 0, 1) != 0)
//         throw std::runtime_error("submit sync read request error.");
    
//     fprintf(stdout, "%s\n", buf);
// }

void sync_read_multiple_thread_test(size_t idx, comm_dev *dev)
{
    char buf[lba_size];
    if (comm_submit_sync_read_request(dev, buf, idx, 1) != 0)
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

int main(int argc, char **argv)
{
    int ret = 0;
    ret = comm_channel_controller_constructor(&dev.channel_ctrlr, &dev, test_channel_size);
    if (ret != 0)
        throw std::runtime_error("channel controller construct failed.");
    if (comm_session_env_init() != 0)
        throw std::runtime_error("session env init failed.");
    polling_thread_arg.dev = &dev;
    th = std::thread(comm_session_polling_thread, static_cast<void*>(&polling_thread_arg));
    th.detach();
    spdk_stub_setup();

    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
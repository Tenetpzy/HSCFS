#include "gtest/gtest.h"
#include "utils/hscfs_timer.h"
#include "../../src/utils/hscfs_timer.c"
#include "../../src/utils/hscfs_log.c"

#include <stdexcept>
#include <sched.h>

class timer_monitor_test: public ::testing::Test
{
protected:
    void SetUp() override
    {
        if (hscfs_timer_monitor_constructor(&t_monitor) != 0)
            throw std::runtime_error("construct monitor failed.");
    }

    void TearDown() override
    {
        hscfs_timer_monitor_destructor(&t_monitor);
    }

    hscfs_timer_monitor t_monitor;
};

void fixed_time_test_cb(uint64_t cnt, void *arg) 
{
    *((uint64_t*)arg) += cnt;
}

// timer基本功能测试
TEST(timer_test, nonblock_test)
{
    hscfs_timer timer;
    uint64_t of_times = 0;
    if (hscfs_timer_constructor(&timer, 0) != 0)
        throw std::runtime_error("construct timer failed.");
    struct timespec tim, cur, after;
    tim.tv_sec = 0;
    tim.tv_nsec = 20 * 1000;
    hscfs_timer_set(&timer, &tim, 0);
    if (hscfs_timer_start(&timer) != 0)
        throw std::runtime_error("start timer failed.");
    clock_gettime(CLOCK_MONOTONIC, &cur);

    uint64_t poll_times = 0;
    while (hscfs_timer_check_expire(&timer, &of_times) == EAGAIN)
        poll_times++;

    clock_gettime(CLOCK_MONOTONIC, &after);
    printf("real time: %lu s %lu ns\noverflow time: %lu\n", after.tv_sec - cur.tv_sec, after.tv_nsec - cur.tv_nsec, of_times);
    printf("polling times: %lu\n", poll_times);
    hscfs_timer_destructor(&timer);
}

TEST(timer_test, block_test)
{
    hscfs_timer timer;
    uint64_t of_times = 0;
    if (hscfs_timer_constructor(&timer, 1) != 0)
        throw std::runtime_error("construct timer failed.");
    struct timespec tim, cur, after;
    tim.tv_sec = 0;
    tim.tv_nsec = 20 * 1000;
    hscfs_timer_set(&timer, &tim, 0);
    if (hscfs_timer_start(&timer) != 0)
        throw std::runtime_error("start timer failed.");
    clock_gettime(CLOCK_MONOTONIC, &cur);

    uint64_t poll_times = 0;
    while (hscfs_timer_check_expire(&timer, &of_times) == EAGAIN)
        poll_times++;

    clock_gettime(CLOCK_MONOTONIC, &after);
    EXPECT_EQ(poll_times, 0);
    printf("real time: %lu s %lu ns\noverflow time: %lu\n", after.tv_sec - cur.tv_sec, after.tv_nsec - cur.tv_nsec, of_times);
    printf("polling times: %lu\n", poll_times);
    hscfs_timer_destructor(&timer);
}


// monitor测试
// TEST_F(timer_monitor_test, fixed_time_test)
// {
//     hscfs_timer timer;
//     uint64_t of_times = 0;
//     if (hscfs_timer_constructor(&timer, 0) != 0)
//         throw std::runtime_error("construct timer failed.");
//     if (hscfs_timer_monitor_add_timer(&t_monitor, &timer, fixed_time_test_cb, &of_times) != 0)
//         throw std::runtime_error("add timer failed.");
//     struct timespec tim, cur, after;
//     tim.tv_sec = 0;
//     tim.tv_nsec = 20 * 1000;  // 10 us
//     hscfs_timer_set(&timer, &tim, 0);
//     if (hscfs_timer_start(&timer) != 0)
//         throw std::runtime_error("start timer failed.");
//     clock_gettime(CLOCK_MONOTONIC, &cur);
//     if (hscfs_timer_monitor_wait_added_timer(&t_monitor) < 0)
//         throw std::runtime_error("wait timer failed.");
//     clock_gettime(CLOCK_MONOTONIC, &after);
//     printf("time : %lu s %lu ns\nof_times: %lu\n", after.tv_sec - cur.tv_sec, after.tv_nsec - cur.tv_nsec, of_times);
//     hscfs_timer_monitor_del_timer(&t_monitor, &timer);
//     hscfs_timer_destructor(&timer);
// }

int main(int argc, char **argv)
{
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
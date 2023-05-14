#include "api/hscfs.hh"
#include "gtest/gtest.h"
#include "host_test_env.hh"

TEST(truncate_test, 1)
{
    int fd = hscfs::open("/a/b/c", O_RDONLY);
    if (fd == -1)
        do_exit("open failed");
    if (hscfs::truncate(fd, 0) != 0)
        do_exit("trunc failed");
    
    char buf[32] = {0};
    ssize_t ret = hscfs::read(fd, buf, sizeof(buf));
    EXPECT_EQ(ret, 0);
    
    if (hscfs::close(fd) != 0)
        do_exit("close failed");
}

int main(int argc, char **argv)
{
    host_test_env_setup();
    ::testing::InitGoogleTest(&argc, argv);
    int ret = RUN_ALL_TESTS();
    host_test_env_teardown();
    return ret;
}
#include "api/hscfs.hh"
#include "gtest/gtest.h"
#include "host_test_env.hh"

TEST(unlink_test, 1)
{
    int fd = hscfs::open("/a/b/c", O_RDONLY);
    if (fd == -1)
        do_exit("open failed");

    int ret, err;
    ASSERT_EQ(hscfs::unlink("/a/b/c"), 0);

    ret = hscfs::unlink("/a/b/c");
    err = errno;
    ASSERT_EQ(ret, -1);
    ASSERT_EQ(err, ENOENT);

    ret = hscfs::unlink("/a");
    err = errno;
    ASSERT_EQ(ret, -1);
    ASSERT_EQ(err, EISDIR);

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
#include "api/hscfs.hh"
#include "gtest/gtest.h"
#include "fmt/ostream.h"
#include <cstdio>
#include <cstring>

void host_test_env_setup();
void host_test_env_teardown();

void do_exit(const char *msg)
{
    perror(msg);
    host_test_env_teardown();
    exit(1);
}

TEST(read_test, 1)
{
    const ssize_t test_file_size = 13;
    const char test_file_content[] = "hello hscfs!";
    int fd = hscfs::open("/a/b/c", O_RDONLY);
    if (fd == -1)
        do_exit("open failed");

    char buf[32] = {0};
    ssize_t ret = hscfs::read(fd, buf, sizeof(buf));
    ASSERT_EQ(ret, test_file_size);
    EXPECT_EQ(strcmp(buf, test_file_content), 0);
}

int main(int argc, char **argv)
{
    host_test_env_setup();
    ::testing::InitGoogleTest(&argc, argv);
    int ret = RUN_ALL_TESTS();
    host_test_env_teardown();
    return ret;
}
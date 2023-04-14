#include "gtest/gtest.h"
#include "fs/path_utils.hh"

using namespace hscfs;

TEST(path_utils, 1)
{
    const char *user_path = "/a/b/c";
    auto path = path_helper::extract_abs_path(user_path);
    auto dir = path_helper::extract_dir_path(path);
    EXPECT_EQ(dir, "/a/b/");
    auto file = path_helper::extract_file_name(path);
    EXPECT_EQ(file, "c");
}

TEST(path_utils, 2)
{
    std::string abs_path("//a/b/c//////");
    auto dir = path_helper::extract_dir_path(abs_path);
    EXPECT_EQ(dir, "//a/b/c//////");
    auto file = path_helper::extract_file_name(abs_path);
    EXPECT_EQ(file, "");

    path_parser parser(abs_path);
    auto itr = parser.begin();
    std::string dentry;
    dentry = itr.get();
    itr.next();
    EXPECT_EQ(dentry, "a");
    dentry = itr.get();
    itr.next();
    EXPECT_EQ(dentry, "b");
    dentry = itr.get();
    itr.next();
    EXPECT_EQ(dentry, "c");
    EXPECT_EQ(itr, parser.end());
}

int main(int argc, char **argv)
{
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}